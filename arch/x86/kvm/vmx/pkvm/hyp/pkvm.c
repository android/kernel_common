// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/hashtable.h>
#include <pkvm.h>

#include "pkvm_hyp.h"
#include "ept.h"
#include "mem_protect.h"
#include "lapic.h"
#include "ptdev.h"

struct pkvm_hyp *pkvm_hyp;

#define MAX_SHADOW_VMS	(PKVM_MAX_NORMAL_VM_NUM + PKVM_MAX_PROTECTED_VM_NUM)
#define HANDLE_OFFSET 1

#define to_shadow_vm_handle(vcpu_handle)	((s64)(vcpu_handle) >> SHADOW_VM_HANDLE_SHIFT)
#define to_shadow_vcpu_idx(vcpu_handle)		((s64)(vcpu_handle) & SHADOW_VCPU_INDEX_MASK)

static DECLARE_BITMAP(shadow_vms_bitmap, MAX_SHADOW_VMS);
static pkvm_spinlock_t shadow_vms_lock = __PKVM_SPINLOCK_UNLOCKED;
struct shadow_vm_ref {
	atomic_t refcount;
	struct pkvm_shadow_vm *vm;
};
static struct shadow_vm_ref shadow_vms_ref[MAX_SHADOW_VMS];

#define SHADOW_VCPU_ARRAY(vm) \
	((struct shadow_vcpu_array *)((void *)(vm) + sizeof(struct pkvm_shadow_vm)))

#define SHADOW_VCPU_HASH_BITS		10
DEFINE_HASHTABLE(shadow_vcpu_table, SHADOW_VCPU_HASH_BITS);
static pkvm_spinlock_t shadow_vcpu_table_lock = __PKVM_SPINLOCK_UNLOCKED;

static int allocate_shadow_vm_handle(struct pkvm_shadow_vm *vm)
{
	struct shadow_vm_ref *vm_ref;
	int handle;

	/*
	 * The shadow_vm_handle is an int so cannot exceed the INT_MAX.
	 * Meanwhile shadow_vm_handle will also be used as owner_id in
	 * the page state machine so it also cannot exceed the max
	 * owner_id.
	 */
	BUILD_BUG_ON(MAX_SHADOW_VMS >
		     min(INT_MAX, ((1 << hweight_long(PKVM_INVALID_PTE_OWNER_MASK)) - 1)));

	pkvm_spin_lock(&shadow_vms_lock);

	handle = find_next_zero_bit(shadow_vms_bitmap, MAX_SHADOW_VMS,
				    HANDLE_OFFSET);
	if ((u32)handle < MAX_SHADOW_VMS) {
		__set_bit(handle, shadow_vms_bitmap);
		vm->shadow_vm_handle = handle;
		vm_ref = &shadow_vms_ref[handle];
		vm_ref->vm = vm;
		atomic_set(&vm_ref->refcount, 1);
	} else
		handle = -ENOMEM;

	pkvm_spin_unlock(&shadow_vms_lock);

	return handle;
}

static struct pkvm_shadow_vm *free_shadow_vm_handle(int handle)
{
	struct shadow_vm_ref *vm_ref;
	struct pkvm_shadow_vm *vm = NULL;

	pkvm_spin_lock(&shadow_vms_lock);

	if ((u32)handle >= MAX_SHADOW_VMS)
		goto out;

	vm_ref = &shadow_vms_ref[handle];
	if ((atomic_cmpxchg(&vm_ref->refcount, 1, 0) != 1)) {
		pkvm_err("%s: VM%d is busy, refcount %d\n",
			 __func__, handle, atomic_read(&vm_ref->refcount));
		goto out;
	}

	vm = vm_ref->vm;

	vm_ref->vm = NULL;
	__clear_bit(handle, shadow_vms_bitmap);
out:
	pkvm_spin_unlock(&shadow_vms_lock);
	return vm;
}

int __pkvm_init_shadow_vm(struct kvm_vcpu *hvcpu, unsigned long kvm_va,
			  unsigned long shadow_pa,  size_t shadow_size)
{
	unsigned long offset = offsetof(struct kvm, arch.vm_type);
	unsigned long vm_type, bytes = sizeof(u8);
	struct pkvm_shadow_vm *vm;
	struct x86_exception e;
	int shadow_vm_handle;

	if (!PAGE_ALIGNED(shadow_pa) ||
		!PAGE_ALIGNED(shadow_size) ||
		(shadow_size != PAGE_ALIGN(sizeof(struct pkvm_shadow_vm)
					   + pkvm_shadow_vcpu_array_size())))
		return -EINVAL;

	if (read_gva(hvcpu, kvm_va + offset, &vm_type, bytes, &e) < 0)
		return -EINVAL;

	if (__pkvm_host_donate_hyp(shadow_pa, shadow_size))
		return -EINVAL;

	vm = pkvm_phys_to_virt(shadow_pa);

	memset(vm, 0, shadow_size);
	pkvm_spin_lock_init(&vm->lock);
	INIT_LIST_HEAD(&vm->ptdev_head);

	vm->host_kvm_va = kvm_va;
	vm->shadow_size = shadow_size;
	vm->vm_type = vm_type;

	if (pkvm_pgstate_pgt_init(vm))
		goto undonate;

	if (pkvm_shadow_ept_init(&vm->sept_desc))
		goto deinit_pgstate_pgt;

	shadow_vm_handle = allocate_shadow_vm_handle(vm);
	if (shadow_vm_handle < 0)
		goto deinit_shadow_ept;

	return shadow_vm_handle;

deinit_shadow_ept:
	pkvm_shadow_ept_deinit(&vm->sept_desc);
deinit_pgstate_pgt:
	pkvm_pgstate_pgt_deinit(vm);
undonate:
	memset(vm, 0, shadow_size);
	__pkvm_hyp_donate_host(shadow_pa, shadow_size);
	return -EINVAL;
}

unsigned long __pkvm_teardown_shadow_vm(int shadow_vm_handle)
{
	struct pkvm_shadow_vm *vm = free_shadow_vm_handle(shadow_vm_handle);
	struct pkvm_ptdev *ptdev, *tmp;
	unsigned long shadow_size;

	if (!vm)
		return 0;

	pkvm_shadow_ept_deinit(&vm->sept_desc);

	pkvm_pgstate_pgt_deinit(vm);

	list_for_each_entry_safe(ptdev, tmp, &vm->ptdev_head, vm_node)
		pkvm_detach_ptdev(ptdev, vm);

	shadow_size = vm->shadow_size;
	memset(vm, 0, shadow_size);

	WARN_ON(__pkvm_hyp_donate_host(pkvm_virt_to_phys(vm), shadow_size));

	return pkvm_virt_to_phys(vm);
}

struct pkvm_shadow_vm *get_shadow_vm(int shadow_vm_handle)
{
	struct shadow_vm_ref *vm_ref;

	if ((u32)shadow_vm_handle >= MAX_SHADOW_VMS)
		return NULL;

	vm_ref = &shadow_vms_ref[shadow_vm_handle];
	return atomic_inc_not_zero(&vm_ref->refcount) ? vm_ref->vm : NULL;
}

void put_shadow_vm(int shadow_vm_handle)
{
	struct shadow_vm_ref *vm_ref;

	if ((u32)shadow_vm_handle >= MAX_SHADOW_VMS)
		return;

	vm_ref = &shadow_vms_ref[shadow_vm_handle];
	WARN_ON(atomic_dec_if_positive(&vm_ref->refcount) <= 0);
}

void pkvm_shadow_vm_link_ptdev(struct pkvm_shadow_vm *vm,
			       struct list_head *node, bool coherency)
{
	pkvm_spin_lock(&vm->lock);
	list_add_tail(node, &vm->ptdev_head);
	vm->noncoherent_ptdev += !coherency;
	vm->need_prepopulation = true;
	pkvm_pgstate_pgt_update_coherency(&vm->pgstate_pgt,
					  !vm->noncoherent_ptdev);
	pkvm_spin_unlock(&vm->lock);
}

void pkvm_shadow_vm_unlink_ptdev(struct pkvm_shadow_vm *vm,
				 struct list_head *node, bool coherency)
{
	pkvm_spin_lock(&vm->lock);
	list_del(node);
	vm->noncoherent_ptdev -= !coherency;
	pkvm_pgstate_pgt_update_coherency(&vm->pgstate_pgt,
					  !vm->noncoherent_ptdev);
	pkvm_spin_unlock(&vm->lock);
}

static void add_shadow_vcpu_vmcs12_map(struct shadow_vcpu_state *vcpu)
{
	pkvm_spin_lock(&shadow_vcpu_table_lock);
	hash_add(shadow_vcpu_table, &vcpu->hnode, vcpu->vmcs12_pa);
	pkvm_spin_unlock(&shadow_vcpu_table_lock);
}

static void remove_shadow_vcpu_vmcs12_map(struct shadow_vcpu_state *vcpu)
{
	pkvm_spin_lock(&shadow_vcpu_table_lock);
	hash_del(&vcpu->hnode);
	pkvm_spin_unlock(&shadow_vcpu_table_lock);
}

s64 find_shadow_vcpu_handle_by_vmcs(unsigned long vmcs12_pa)
{
	struct shadow_vcpu_state *shadow_vcpu;
	s64 handle = -1;

	pkvm_spin_lock(&shadow_vcpu_table_lock);
	hash_for_each_possible(shadow_vcpu_table, shadow_vcpu, hnode, vmcs12_pa) {
		if (shadow_vcpu->vmcs12_pa == vmcs12_pa) {
			handle = shadow_vcpu->shadow_vcpu_handle;
			break;
		}
	}
	pkvm_spin_unlock(&shadow_vcpu_table_lock);

	return handle;
}

struct shadow_vcpu_state *get_shadow_vcpu(s64 shadow_vcpu_handle)
{
	int shadow_vm_handle = to_shadow_vm_handle(shadow_vcpu_handle);
	u32 vcpu_idx = to_shadow_vcpu_idx(shadow_vcpu_handle);
	struct shadow_vcpu_ref *vcpu_ref;
	struct shadow_vcpu_state *vcpu;
	struct pkvm_shadow_vm *vm;

	if (vcpu_idx >= KVM_MAX_VCPUS)
		return NULL;

	vm = get_shadow_vm(shadow_vm_handle);
	if (!vm)
		return NULL;

	vcpu_ref = &SHADOW_VCPU_ARRAY(vm)->ref[vcpu_idx];
	vcpu = atomic_inc_not_zero(&vcpu_ref->refcount) ? vcpu_ref->vcpu : NULL;

	put_shadow_vm(shadow_vm_handle);
	return vcpu;
}

void put_shadow_vcpu(s64 shadow_vcpu_handle)
{
	int shadow_vm_handle = to_shadow_vm_handle(shadow_vcpu_handle);
	u32 vcpu_idx = to_shadow_vcpu_idx(shadow_vcpu_handle);
	struct shadow_vcpu_ref *vcpu_ref;
	struct pkvm_shadow_vm *vm;

	if (vcpu_idx >= KVM_MAX_VCPUS)
		return;

	vm = get_shadow_vm(shadow_vm_handle);
	if (!vm)
		return;

	vcpu_ref = &SHADOW_VCPU_ARRAY(vm)->ref[vcpu_idx];
	WARN_ON(atomic_dec_if_positive(&vcpu_ref->refcount) <= 0);

	put_shadow_vm(shadow_vm_handle);
}

static s64 attach_shadow_vcpu_to_vm(struct pkvm_shadow_vm *vm,
				    struct shadow_vcpu_state *shadow_vcpu)
{
	struct shadow_vcpu_ref *vcpu_ref;
	u32 vcpu_idx;

	/*
	 * Shadow_vcpu_handle is a s64 value combined with shadow_vm_handle
	 * and shadow_vcpu index from the array. So the array size cannot be
	 * larger than the shadow_vcpu index mask.
	 */
	BUILD_BUG_ON(KVM_MAX_VCPUS > SHADOW_VCPU_INDEX_MASK);

	/*
	 * Save a shadow_vm pointer in shadow_vcpu requires additional
	 * get so that later when use this pointer at runtime no need
	 * to get again. This will be put when detaching this shadow_vcpu.
	 */
	shadow_vcpu->vm = get_shadow_vm(vm->shadow_vm_handle);
	if (!shadow_vcpu->vm)
		return -EINVAL;

	add_shadow_vcpu_vmcs12_map(shadow_vcpu);

	pkvm_spin_lock(&vm->lock);

	if (vm->created_vcpus == KVM_MAX_VCPUS) {
		pkvm_spin_unlock(&vm->lock);
		return -EINVAL;
	}

	vcpu_idx = vm->created_vcpus;
	shadow_vcpu->shadow_vcpu_handle =
		to_shadow_vcpu_handle(vm->shadow_vm_handle, vcpu_idx);
	vcpu_ref = &SHADOW_VCPU_ARRAY(vm)->ref[vcpu_idx];
	vcpu_ref->vcpu = shadow_vcpu;
	vm->created_vcpus++;
	atomic_set(&vcpu_ref->refcount, 1);

	pkvm_spin_unlock(&vm->lock);

	return shadow_vcpu->shadow_vcpu_handle;
}

static struct shadow_vcpu_state *
detach_shadow_vcpu_from_vm(struct pkvm_shadow_vm *vm, s64 shadow_vcpu_handle)
{
	u32 vcpu_idx = to_shadow_vcpu_idx(shadow_vcpu_handle);
	struct shadow_vcpu_state *shadow_vcpu = NULL;
	struct shadow_vcpu_ref *vcpu_ref;

	if (vcpu_idx >= KVM_MAX_VCPUS)
		return NULL;

	pkvm_spin_lock(&vm->lock);

	vcpu_ref = &SHADOW_VCPU_ARRAY(vm)->ref[vcpu_idx];
	if ((atomic_cmpxchg(&vcpu_ref->refcount, 1, 0) != 1)) {
		pkvm_err("%s: VM%d shadow_vcpu%d is busy, refcount %d\n",
			 __func__, vm->shadow_vm_handle, vcpu_idx,
			 atomic_read(&vcpu_ref->refcount));
	} else {
		shadow_vcpu = vcpu_ref->vcpu;
		vcpu_ref->vcpu = NULL;
	}

	pkvm_spin_unlock(&vm->lock);

	if (shadow_vcpu) {
		remove_shadow_vcpu_vmcs12_map(shadow_vcpu);
		/*
		 * Paired with the get_shadow_vm when saving the shadow_vm pointer
		 * during attaching shadow_vcpu.
		 */
		put_shadow_vm(shadow_vcpu->vm->shadow_vm_handle);
	}

	return shadow_vcpu;
}

s64 __pkvm_init_shadow_vcpu(struct kvm_vcpu *hvcpu, int shadow_vm_handle,
			    unsigned long vcpu_va, unsigned long shadow_pa,
			    size_t shadow_size)
{
	struct pkvm_shadow_vm *vm;
	struct shadow_vcpu_state *shadow_vcpu;
	struct x86_exception e;
	unsigned long vmcs12_va;
	s64 shadow_vcpu_handle;
	int ret;

	if (!PAGE_ALIGNED(shadow_pa) || !PAGE_ALIGNED(shadow_size) ||
		(shadow_size != PAGE_ALIGN(sizeof(struct shadow_vcpu_state))) ||
		(vmx_basic_vmcs_size(pkvm_hyp->vmcs_config.basic) > PAGE_SIZE))
		return -EINVAL;

	if (__pkvm_host_donate_hyp(shadow_pa, shadow_size))
		return -EINVAL;

	shadow_vcpu = pkvm_phys_to_virt(shadow_pa);
	memset(shadow_vcpu, 0, shadow_size);
	shadow_vcpu->shadow_size = shadow_size;

	ret = read_gva(hvcpu, vcpu_va, &shadow_vcpu->vmx, sizeof(struct vcpu_vmx), &e);
	if (ret < 0)
		goto undonate;

	vmcs12_va = (unsigned long)shadow_vcpu->vmx.vmcs01.vmcs;
	if (gva2gpa(hvcpu, vmcs12_va, (gpa_t *)&shadow_vcpu->vmcs12_pa, 0, &e))
		goto undonate;

	vm = get_shadow_vm(shadow_vm_handle);
	if (!vm)
		goto undonate;

	shadow_vcpu_handle = attach_shadow_vcpu_to_vm(vm, shadow_vcpu);

	put_shadow_vm(shadow_vm_handle);

	if (shadow_vcpu_handle < 0)
		goto undonate;

	return shadow_vcpu_handle;
undonate:
	memset(shadow_vcpu, 0, shadow_size);
	__pkvm_hyp_donate_host(shadow_pa, shadow_size);
	return -EINVAL;
}

unsigned long __pkvm_teardown_shadow_vcpu(s64 shadow_vcpu_handle)
{
	int shadow_vm_handle = to_shadow_vm_handle(shadow_vcpu_handle);
	struct shadow_vcpu_state *shadow_vcpu;
	unsigned long shadow_size;
	struct pkvm_shadow_vm *vm = get_shadow_vm(shadow_vm_handle);

	if (!vm)
		return 0;

	shadow_vcpu = detach_shadow_vcpu_from_vm(vm, shadow_vcpu_handle);

	put_shadow_vm(shadow_vm_handle);

	if (!shadow_vcpu)
		return 0;

	shadow_size = shadow_vcpu->shadow_size;
	memset(shadow_vcpu, 0, shadow_size);
	WARN_ON(__pkvm_hyp_donate_host(pkvm_virt_to_phys(shadow_vcpu),
				       shadow_size));

	return pkvm_virt_to_phys(shadow_vcpu);
}

void pkvm_kick_vcpu(struct kvm_vcpu *vcpu)
{
	struct pkvm_host_vcpu *hvcpu = to_pkvm_hvcpu(vcpu);
	struct pkvm_pcpu *pcpu = hvcpu->pcpu;

	if (kvm_vcpu_exiting_guest_mode(vcpu) != IN_GUEST_MODE)
		return;

	pkvm_lapic_send_init(pcpu);
}

int pkvm_add_ptdev(int shadow_vm_handle, u16 bdf, u32 pasid)
{
	struct pkvm_shadow_vm *vm = get_shadow_vm(shadow_vm_handle);
	int ret = 0;

	if (!vm)
		return -EINVAL;

	if (vm->vm_type != KVM_X86_DEFAULT_VM)
		ret = pkvm_attach_ptdev(bdf, pasid, vm);

	put_shadow_vm(shadow_vm_handle);

	return ret;
}
