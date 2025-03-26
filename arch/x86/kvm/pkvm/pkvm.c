// SPDX-License-Identifier: GPL-2.0
#include <asm/processor.h>
#include <asm/kvm_pkvm.h>
#include "pkvm.h"
#include <asm/pkvm_spinlock.h>
//FIXME: clean up the header files
#include <vmx/pkvm/hyp/mem_protect.h>
#include <vmx/pkvm/hyp/memory.h>

struct cpuinfo_x86 boot_cpu_data;
unsigned int tsc_khz;

/*
 * FIXME: This was defined in kvm/mmu/mmu.c but as this file is not used for
 * adding cpu state protection, there is no equivalent mmu.c in the pkvm
 * hypervisor, define the tdp_enabled here to simplify.
 */
bool tdp_enabled = true;
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);

size_t pkvm_vm_sz = sizeof(struct pkvm_vm);

static DECLARE_BITMAP(pkvm_vms_bitmap, MAX_PKVM_VMS);
static pkvm_spinlock_t pkvm_vms_lock = __PKVM_SPINLOCK_UNLOCKED;
static struct pkvm_vm_ref pkvm_vms_ref[MAX_PKVM_VMS];

#define HANDLE_OFFSET 1

static int idx_to_vm_handle(int idx)
{
	return idx + HANDLE_OFFSET;
}

static int vm_handle_to_idx(int handle)
{
	return handle - HANDLE_OFFSET;
}

static int allocate_pkvm_vm_handle(struct pkvm_vm *pkvm_vm)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	int idx;

	/*
	 * The pkvm_vm_handle is an int so cannot exceed the INT_MAX.
	 * Meanwhile pkvm_vm_handle will also be used as owner_id in
	 * the page state machine so it also cannot exceed the max
	 * owner_id.
	 */
	BUILD_BUG_ON(MAX_PKVM_VMS >
		     min(INT_MAX, ((1 << hweight_long(PKVM_INVALID_PTE_OWNER_MASK)) - 1)));

	pkvm_spin_lock(&pkvm_vms_lock);

	idx = find_next_zero_bit(pkvm_vms_bitmap, MAX_PKVM_VMS, 0);
	if (idx == MAX_PKVM_VMS) {
		pkvm_spin_unlock(&pkvm_vms_lock);
		return -ENOMEM;
	}
	__set_bit(idx, pkvm_vms_bitmap);

	to_kvm(pkvm_vm)->arch.pkvm.shadow_vm_handle = idx_to_vm_handle(idx);
	pkvm_vm_ref = &pkvm_vms_ref[idx];
	pkvm_vm_ref->pkvm_vm = pkvm_vm;
	atomic_set(&pkvm_vm_ref->refcount, 1);

	pkvm_spin_unlock(&pkvm_vms_lock);
	return 0;
}

static struct pkvm_vm *free_pkvm_vm_handle(int handle)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	struct pkvm_vm *pkvm_vm = NULL;
	int idx;

	idx = vm_handle_to_idx(handle);
	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	pkvm_spin_lock(&pkvm_vms_lock);

	pkvm_vm_ref = &pkvm_vms_ref[idx];
	if ((atomic_cmpxchg(&pkvm_vm_ref->refcount, 1, 0) != 1)) {
		pr_err("%s: VM%d is busy, refcount %d\n",
			__func__, handle, atomic_read(&pkvm_vm_ref->refcount));
		goto out;
	}

	pkvm_vm = pkvm_vm_ref->pkvm_vm;
	pkvm_vm_ref->pkvm_vm = NULL;

	__clear_bit(idx, pkvm_vms_bitmap);
out:
	pkvm_spin_unlock(&pkvm_vms_lock);
	return pkvm_vm;
}

static void teardown_donated_memory(struct pkvm_memcache *mc, void *addr, size_t size)
{
	size = PAGE_ALIGN(size);
	memset(addr, 0, size);

	for (void *start = addr; start < addr + size; start += PAGE_SIZE)
		push_pkvm_memcache(mc, start, pkvm_virt_to_host_gpa);

	__pkvm_hyp_donate_host(pkvm_virt_to_phys(addr), size);
}

static int pkvm_vm_init(struct kvm *shared_kvm, unsigned long gpa)
{
	unsigned long pkvm_vm_pa;
	struct pkvm_vm *pkvm_vm;
	struct kvm *kvm;
	size_t pa_size;
	int ret;

	pkvm_vm_pa = host_gpa2hpa(gpa);
	if (!PAGE_ALIGNED(pkvm_vm_pa))
		return -EINVAL;

	pa_size = PAGE_ALIGN(pkvm_vm_sz);
	if (__pkvm_host_donate_hyp(pkvm_vm_pa, pa_size))
		return -EINVAL;

	pkvm_vm = pkvm_phys_to_virt(pkvm_vm_pa);
	memset(pkvm_vm, 0, pa_size);

	pkvm_vm->size = pa_size;
	/*
	 * TODO: Assume host is already share the kvm structure
	 * (represented by shared_kvm) with pkvm. So just pin
	 * shared_kvm.
	 */
	pkvm_vm->shared_kvm = shared_kvm;
	pkvm_vm->lock = __PKVM_SPINLOCK_UNLOCKED;

	kvm = to_kvm(pkvm_vm);

	ret = kvm_arch_init_vm(kvm, pkvm_vm->shared_kvm->arch.vm_type);
	if (ret)
		goto undonate;

	ret = allocate_pkvm_vm_handle(pkvm_vm);
	if (ret)
		goto vm_destroy;

	return kvm->arch.pkvm.shadow_vm_handle;

vm_destroy:
	kvm_x86_call(vm_destroy)(kvm);
undonate:
	__pkvm_hyp_donate_host(pkvm_vm_pa, pa_size);
	return ret;
}

static void pkvm_vm_destroy(int handle)
{
	struct kvm_protected_vm *shared_pkvm;
	struct pkvm_vm *pkvm_vm;

	pkvm_vm = free_pkvm_vm_handle(handle);
	if (!pkvm_vm)
		return;
	shared_pkvm = &pkvm_vm->shared_kvm->arch.pkvm;

	kvm_arch_destroy_vm(to_kvm(pkvm_vm));
	teardown_donated_memory(&shared_pkvm->teardown_mc,
				(void *)pkvm_vm, pkvm_vm->size);

	/* TODO: unpin shared_kvm */
}

unsigned long handle_kvm_call(unsigned long fn, unsigned long p1,
			      unsigned long p2, unsigned long p3)
{
	unsigned long ret = 0;

	switch (fn) {
	case __pkvm__enable_virtualization_cpu:
		ret = kvm_arch_enable_virtualization_cpu();
		break;
	case __pkvm__disable_virtualization_cpu:
		kvm_arch_disable_virtualization_cpu();
		break;
	case __pkvm__check_processor_compatibility:
		ret = kvm_x86_call(check_processor_compatibility)();
		break;
	case __pkvm__vm_init:
		ret = pkvm_vm_init((struct kvm *)kern_pkvm_va((void *)p1), p2);
		break;
	case __pkvm__vm_destroy:
		pkvm_vm_destroy((int)p1);
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
