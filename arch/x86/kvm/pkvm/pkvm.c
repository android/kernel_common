// SPDX-License-Identifier: GPL-2.0
#include <asm/processor.h>
#include <asm/kvm_pkvm.h>
#include "x86.h"
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
u64 x86_pred_cmd;
DEFINE_PER_CPU(u64, x86_spec_ctrl_current);

size_t pkvm_vm_sz = sizeof(struct pkvm_vm);
size_t pkvm_vcpu_sz = sizeof(struct pkvm_vcpu);

static DECLARE_BITMAP(pkvm_vms_bitmap, MAX_PKVM_VMS);
static pkvm_spinlock_t pkvm_vms_lock = __PKVM_SPINLOCK_UNLOCKED;
static struct pkvm_vm_ref pkvm_vms_ref[MAX_PKVM_VMS];
struct pkvm_x86_ops pkvm_x86_ops __read_mostly;

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

	to_kvm(pkvm_vm)->arch.pkvm.pkvm_vm_handle = idx_to_vm_handle(idx);
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

	return kvm->arch.pkvm.pkvm_vm_handle;

vm_destroy:
	kvm_x86_call(vm_destroy)(kvm);
undonate:
	__pkvm_hyp_donate_host(pkvm_vm_pa, pa_size);
	return ret;
}

static int attach_pkvm_vcpu_to_vm(struct pkvm_vcpu *pkvm_vcpu, struct pkvm_vm *pkvm_vm)
{
	struct kvm_vcpu *vcpu;
	struct kvm *kvm;
	int ret = 0;

	kvm = to_kvm(pkvm_vm);

	pkvm_spin_lock(&pkvm_vm->lock);

	if (kvm->created_vcpus == KVM_MAX_VCPUS) {
		ret = -EINVAL;
		goto out;
	}

	pkvm_vcpu->vcpu_idx = kvm->created_vcpus;
	pkvm_vcpu->pkvm_vm = pkvm_vm;

	/* Setup necessary fields in kvm_vcpu */
	vcpu = to_kvm_vcpu(pkvm_vcpu);
	/* Set cpu to -1 to indicate it is not loaded on any CPU */
	vcpu->cpu = -1;
	vcpu->kvm = kvm;
	vcpu->vcpu_id = pkvm_vcpu->shared_vcpu->vcpu_id;
	/*
	 * Set apic in vcpu->arch points to the apic in the shared vcpu
	 * to make the pkvm hypervisor knows if lapic_in_kernel() is true or
	 * not. The pkvm hypervisor should not use this as a normal memory.
	 */
	vcpu->arch.apic = pkvm_vcpu->shared_vcpu->arch.apic;
	vcpu->arch.apic_base = pkvm_vcpu->shared_vcpu->arch.apic_base;

	ret = kvm_arch_vcpu_create(vcpu);
	if (ret)
		goto out;

	pkvm_vm->vcpus[pkvm_vcpu->vcpu_idx] = pkvm_vcpu;
	kvm->created_vcpus++;
out:
	pkvm_spin_unlock(&pkvm_vm->lock);
	return ret;
}

static void detach_pkvm_vcpu_from_vm(struct pkvm_vcpu *pkvm_vcpu, struct pkvm_vm *pkvm_vm)
{
	struct kvm *kvm;

	kvm = to_kvm(pkvm_vm);

	pkvm_spin_lock(&pkvm_vm->lock);

	kvm_x86_call(vcpu_free)(to_kvm_vcpu(pkvm_vcpu));

	pkvm_vm->vcpus[pkvm_vcpu->vcpu_idx] = NULL;

	pkvm_spin_unlock(&pkvm_vm->lock);
}

struct pkvm_vm *get_pkvm_vm(int handle)
{
	int idx = vm_handle_to_idx(handle);
	struct pkvm_vm_ref *pkvm_vm_ref;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	pkvm_vm_ref = &pkvm_vms_ref[idx];
	return atomic_inc_not_zero(&pkvm_vm_ref->refcount) ? pkvm_vm_ref->pkvm_vm : NULL;
}

void put_pkvm_vm(struct pkvm_vm *pkvm_vm)
{
	int idx = vm_handle_to_idx(to_kvm(pkvm_vm)->arch.pkvm.pkvm_vm_handle);
	struct pkvm_vm_ref *pkvm_vm_ref;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return;

	pkvm_vm_ref = &pkvm_vms_ref[idx];
	WARN_ON(atomic_dec_if_positive(&pkvm_vm_ref->refcount) <= 0);
}

static int pkvm_vcpu_create(struct kvm_vcpu *shared_vcpu, unsigned long gpa)
{
	struct pkvm_vcpu *pkvm_vcpu;
	unsigned long pkvm_vcpu_pa;
	struct pkvm_vm *pkvm_vm;
	struct kvm *shared_kvm;
	size_t pa_size;
	int ret;

	pkvm_vcpu_pa = host_gpa2hpa(gpa);
	if (!PAGE_ALIGNED(pkvm_vcpu_pa))
		return -EINVAL;

	pa_size = PAGE_ALIGN(pkvm_vcpu_sz);
	if (__pkvm_host_donate_hyp(pkvm_vcpu_pa, pa_size))
		return -EINVAL;

	pkvm_vcpu = pkvm_phys_to_virt(pkvm_vcpu_pa);
	memset(pkvm_vcpu, 0, pa_size);

	pkvm_vcpu->size = pa_size;
	/*
	 * TODO: Assume host is already share the kvm_vcpu structure
	 * (represented by shared_vcpu) with pkvm. So just pin
	 * shared_vcpu. Unpin shared_vcpu when destroying
	 */
	pkvm_vcpu->shared_vcpu = shared_vcpu;

	shared_kvm = kern_pkvm_va(pkvm_vcpu->shared_vcpu->kvm);
	pkvm_vm = get_pkvm_vm(shared_kvm->arch.pkvm.pkvm_vm_handle);
	if (!pkvm_vm) {
		ret = -EBUSY;
		goto undonate;
	}

	ret = attach_pkvm_vcpu_to_vm(pkvm_vcpu, pkvm_vm);
	if (ret)
		goto put_pkvm_vm;

	put_pkvm_vm(pkvm_vm);

	return pkvm_vcpu->vcpu_idx;

put_pkvm_vm:
	put_pkvm_vm(pkvm_vm);
undonate:
	__pkvm_hyp_donate_host(pkvm_vcpu_pa, pa_size);
	return ret;
}

static void pkvm_vm_destroy(int handle)
{
	struct kvm_protected_vm *shared_pkvm;
	struct pkvm_vm *pkvm_vm;
	int i;

	pkvm_vm = free_pkvm_vm_handle(handle);
	if (!pkvm_vm)
		return;
	shared_pkvm = &pkvm_vm->shared_kvm->arch.pkvm;

	for (i = 0; i < to_kvm(pkvm_vm)->created_vcpus; i++) {
		struct pkvm_vcpu *pkvm_vcpu = pkvm_vm->vcpus[i];

		detach_pkvm_vcpu_from_vm(pkvm_vcpu, pkvm_vm);
		teardown_donated_memory(&shared_pkvm->teardown_mc,
					(void *)pkvm_vcpu, pkvm_vcpu->size);
		/* TODO: unpin shared kvm_vcpu */
	}

	kvm_arch_destroy_vm(to_kvm(pkvm_vm));
	teardown_donated_memory(&shared_pkvm->teardown_mc,
				(void *)pkvm_vm, pkvm_vm->size);

	/* TODO: unpin shared_kvm */
}

static struct pkvm_vcpu *get_pkvm_vcpu_from_vm(struct pkvm_vm *pkvm_vm, int handle)
{
	struct pkvm_vcpu *pkvm_vcpu = NULL;

	pkvm_spin_lock(&pkvm_vm->lock);

	if (handle < to_kvm(pkvm_vm)->created_vcpus)
		pkvm_vcpu = pkvm_vm->vcpus[handle];

	pkvm_spin_unlock(&pkvm_vm->lock);

	return pkvm_vcpu;
}

struct pkvm_vcpu *get_pkvm_vcpu(int vm_handle, int vcpu_handle)
{
	struct pkvm_vcpu *pkvm_vcpu;
	struct pkvm_vm *pkvm_vm;

	pkvm_vm = get_pkvm_vm(vm_handle);
	if (!pkvm_vm)
		return NULL;

	pkvm_vcpu = get_pkvm_vcpu_from_vm(pkvm_vm, vcpu_handle);
	if (!pkvm_vcpu)
		put_pkvm_vm(pkvm_vm);

	return pkvm_vcpu;
}

void put_pkvm_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	put_pkvm_vm(pkvm_vcpu->pkvm_vm);
}

static struct pkvm_vcpu *get_pkvm_vcpu_via_shared(struct kvm_vcpu *shared_vcpu)
{
	struct pkvm_vcpu *pkvm_vcpu;
	struct kvm *shared_kvm;

	/*
	 * FIXME: Any check neeeds to be performed before accessing
	 * the shared_vcpu?
	 */
	shared_kvm = kern_pkvm_va(shared_vcpu->kvm);
	pkvm_vcpu = get_pkvm_vcpu(shared_kvm->arch.pkvm.pkvm_vm_handle,
				  shared_vcpu->arch.pkvm_vcpu_handle);
	if (!pkvm_vcpu)
		return NULL;

	if (pkvm_vcpu->shared_vcpu == shared_vcpu)
		return pkvm_vcpu;

	put_pkvm_vcpu(pkvm_vcpu);
	return NULL;
}

static struct pkvm_vcpu *load_pkvm_vcpu(struct kvm_vcpu *shared_vcpu)
{
	struct pkvm_vcpu *pkvm_vcpu = get_pkvm_vcpu_via_shared(shared_vcpu);

	if (!pkvm_vcpu)
		return NULL;

	pkvm_x86_call(switch_to_guest_vcpu)(to_kvm_vcpu(pkvm_vcpu));

	return pkvm_vcpu;
}

static void unload_pkvm_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	if (!pkvm_vcpu)
		return;

	pkvm_x86_call(switch_to_host_vcpu)(to_kvm_vcpu(pkvm_vcpu));

	put_pkvm_vcpu(pkvm_vcpu);
}

static void set_pkvm_vcpu_inuse(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);
	int cpu = raw_smp_processor_id();

	if (vcpu->cpu != -1)
		/* Already inuse */
		return;

	get_pkvm_vm(vcpu->kvm->arch.pkvm.pkvm_vm_handle);
	vcpu->cpu = cpu;
}

static void set_pkvm_vcpu_free(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);

	if (vcpu->cpu != raw_smp_processor_id())
		/* Not inuse in this CPU */
		return;

	vcpu->cpu = -1;
	put_pkvm_vm(pkvm_vcpu->pkvm_vm);
}

static void pkvm_vcpu_load(struct pkvm_vcpu *pkvm_vcpu, int cpu)
{
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(cpu != raw_smp_processor_id()))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);

	/*
	 * Loading vcpu on a new CPU without putting it from the previous one
	 * is not supported.
	 */
	if (WARN_ON_ONCE(vcpu->cpu != -1 && vcpu->cpu != cpu))
		return;

	kvm_x86_call(vcpu_load)(vcpu, cpu);

	set_pkvm_vcpu_inuse(pkvm_vcpu);
}

static void pkvm_vcpu_put(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);

	/* Only the CPU which has loaded this vcpu can do a put */
	if (WARN_ON_ONCE(vcpu->cpu != raw_smp_processor_id()))
		return;

	kvm_x86_call(vcpu_put)(vcpu);

	set_pkvm_vcpu_free(pkvm_vcpu);
}

static void pkvm_vcpu_run(struct pkvm_vcpu *pkvm_vcpu, bool force_immediate_exit)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_vcpu_enter_guest(to_kvm_vcpu(pkvm_vcpu), force_immediate_exit);
}

static unsigned long pkvm_vcpu_handle_kvm_call(unsigned long fn,
					       struct kvm_vcpu *shared_vcpu,
					       unsigned long p2, unsigned  long p3)
{
	struct pkvm_vcpu *pkvm_vcpu;
	unsigned long ret = 0;

	pkvm_vcpu = load_pkvm_vcpu(shared_vcpu);

	switch (fn) {
	case __pkvm__vcpu_load:
		pkvm_vcpu_load(pkvm_vcpu, (int)p2);
		break;
	case __pkvm__vcpu_put:
		pkvm_vcpu_put(pkvm_vcpu);
		break;
	case __pkvm__vcpu_run:
		pkvm_vcpu_run(pkvm_vcpu, (bool)p2);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	unload_pkvm_vcpu(pkvm_vcpu);

	return ret;
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
	case __pkvm__vcpu_create:
		ret = pkvm_vcpu_create((struct kvm_vcpu *)kern_pkvm_va((void *)p1), p2);
		break;
	default:
		ret = pkvm_vcpu_handle_kvm_call(fn, (struct kvm_vcpu *)kern_pkvm_va((void *)p1),
						p2, p3);
		break;
	}

	return ret;
}

void pkvm_x86_ops_init(struct pkvm_x86_ops *ops)
{
	memcpy(&pkvm_x86_ops, ops, sizeof(struct pkvm_x86_ops));
}
