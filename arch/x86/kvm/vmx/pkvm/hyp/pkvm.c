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
#include "memory.h"
#include "debug.h"
#include <pkvm/pkvm.h>
#include <pkvm/vmx/vmx.h>

struct pkvm_hyp *pkvm_hyp;

#define to_shadow_vm_handle(vcpu_handle)	((s64)(vcpu_handle) >> SHADOW_VM_HANDLE_SHIFT)
#define to_shadow_vcpu_idx(vcpu_handle)		((s64)(vcpu_handle) & SHADOW_VCPU_INDEX_MASK)

#define SHADOW_VCPU_HASH_BITS		10
DEFINE_HASHTABLE(shadow_vcpu_table, SHADOW_VCPU_HASH_BITS);
static pkvm_spinlock_t shadow_vcpu_table_lock = __PKVM_SPINLOCK_UNLOCKED;

int __pkvm_finalize_shadow_vm(int shadow_vm_handle, int primary_vcpu_handle,
			      gpa_t pvmfw_load_addr)
{
	struct pkvm_shadow_vm *vm;
	struct pkvm_vm *pkvm_vm;
	int idx;
	int ret = 0;

	pkvm_vm = get_pkvm_vm(shadow_vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	vm = kvm_to_shadow(to_kvm(pkvm_vm));
	pkvm_spin_lock(&vm->lock);

	if (vm->finalized) {
		ret = -EBUSY;
		goto unlock;
	}

	if (pvmfw_load_addr != PVMFW_INVALID_LOAD_ADDR) {
		if (!pvmfw_present) {
			ret = -EINVAL;
			goto unlock;
		}
		if (!shadow_vm_is_protected(vm)) {
			ret = -EPERM;
			goto unlock;
		}

		vm->pvmfw_load_addr = pvmfw_load_addr;
	}

	for (idx = 0; idx < to_kvm(pkvm_vm)->created_vcpus; idx++) {
		struct pkvm_vcpu *pkvm_vcpu = get_pkvm_vcpu(shadow_vm_handle, idx);
		struct shadow_vcpu_state *shadow_vcpu;

		if (!pkvm_vcpu)
			continue;

		shadow_vcpu = kvm_vcpu_to_shadow(to_kvm_vcpu(pkvm_vcpu));
		if (pkvm_vcpu->vcpu_idx == primary_vcpu_handle &&
		    vm->pvmfw_load_addr != PVMFW_INVALID_LOAD_ADDR) {
			/*
			 * If a protected VM is running with pvmfw, enforce the pvmfw
			 * as the VM entry point on its primary vCPU.
			 *
			 * TODO: also need to prevent running secondary vCPUs
			 * until the VM itself allows it (probably via a hypercall
			 * to pKVM) at the moment when it boots a secondary vCPU.
			 */
			shadow_vcpu->pvmfw_entry_pending = true;
		}

		/*
		 * Make sure to update pvmfw_entry_pending and pvmfw_load_addr
		 * before allowing primary vCPU to run. Paired with __smp_rmb()
		 * in nested_vmx_run().
		 */
		__smp_wmb();
		WRITE_ONCE(shadow_vcpu->allowed_to_run, true);

		put_pkvm_vcpu(pkvm_vcpu);
	}

	vm->finalized = true;

unlock:
	pkvm_spin_unlock(&vm->lock);

	put_pkvm_vm(pkvm_vm);
	return ret;
}

void pkvm_shadow_vm_link_ptdev(struct pkvm_shadow_vm *vm,
			       struct list_head *node, bool coherency)
{
	pkvm_spin_lock(&vm->lock);
	list_add_tail(node, &vm->ptdev_head);
	vm->noncoherent_ptdev += !coherency;
	vm->need_prepopulation = true;
	pkvm_shadow_sl_iommu_pgt_update_coherency(&vm->pgstate_pgt,
						  !vm->noncoherent_ptdev);
	pkvm_spin_unlock(&vm->lock);
}

void pkvm_shadow_vm_unlink_ptdev(struct pkvm_shadow_vm *vm,
				 struct list_head *node, bool coherency)
{
	pkvm_spin_lock(&vm->lock);
	list_del(node);
	vm->noncoherent_ptdev -= !coherency;
	pkvm_shadow_sl_iommu_pgt_update_coherency(&vm->pgstate_pgt,
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
	int vm_handle = to_shadow_vm_handle(shadow_vcpu_handle);
	int vcpu_handle = to_shadow_vcpu_idx(shadow_vcpu_handle);
	struct pkvm_vcpu *pkvm_vcpu;

	pkvm_vcpu = get_pkvm_vcpu(vm_handle, vcpu_handle);
	if (!pkvm_vcpu)
		return NULL;

	return kvm_vcpu_to_shadow(to_kvm_vcpu(pkvm_vcpu));
}

void put_shadow_vcpu(struct shadow_vcpu_state *shadow_vcpu)
{
	struct kvm_vcpu *vcpu = shadow_to_kvm_vcpu(shadow_vcpu);

	put_pkvm_vcpu(to_pkvm_vcpu(vcpu));
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
	struct pkvm_vm *pkvm_vm = get_pkvm_vm(shadow_vm_handle);
	struct pkvm_shadow_vm *vm;
	int ret = 0;

	if (!pkvm_vm)
		return -EINVAL;

	vm = kvm_to_shadow(to_kvm(pkvm_vm));

	if (shadow_vm_is_protected(vm))
		ret = pkvm_attach_ptdev(bdf, pasid, vm);

	put_pkvm_vm(pkvm_vm);

	return ret;
}

int pkvm_load_pvmfw_pages(struct pkvm_shadow_vm *vm, u64 gpa, u64 phys, u64 size)
{
	u64 offset = gpa - vm->pvmfw_load_addr;

	if (offset >= pvmfw_size)
		return -EINVAL;

	size = min(size, pvmfw_size - offset);
	if (!PAGE_ALIGNED(size) || !PAGE_ALIGNED(offset))
		return -EINVAL;

	memcpy(__pkvm_va(phys), __pkvm_va(pvmfw_base + offset), size);
	return 0;
}

int pkvm_init_shadow_vm(struct kvm *kvm)
{
	struct pkvm_shadow_vm *vm = kvm_to_shadow(kvm);
	int ret;

	pkvm_spin_lock_init(&vm->lock);
	INIT_LIST_HEAD(&vm->ptdev_head);
	vm->vm_type = kvm->arch.vm_type;
	vm->pvmfw_load_addr = PVMFW_INVALID_LOAD_ADDR;
	vm->finalized = !shadow_vm_is_protected(vm);

	ret = pkvm_pgstate_pgt_init(vm);
	if (ret)
		goto out;

	ret = pkvm_shadow_ept_init(&vm->sept_desc);
	if (ret)
		goto deinit_pgstate_pgt;

	return 0;

deinit_pgstate_pgt:
	pkvm_pgstate_pgt_deinit(vm);
out:
	return ret;
}

void pkvm_teardown_shadow_vm(struct kvm *kvm)
{
	struct pkvm_shadow_vm *vm = kvm_to_shadow(kvm);
	struct pkvm_ptdev *ptdev, *tmp;

	pkvm_shadow_ept_deinit(&vm->sept_desc);
	pkvm_pgstate_pgt_deinit(vm);

	list_for_each_entry_safe(ptdev, tmp, &vm->ptdev_head, vm_node)
		pkvm_detach_ptdev(ptdev, vm);
}

int pkvm_init_shadow_vcpu(struct kvm_vcpu *vcpu)
{
	struct shadow_vcpu_state *shadow_vcpu = kvm_vcpu_to_shadow(vcpu);
	int vm_handle = vcpu->kvm->arch.pkvm.pkvm_vm_handle;
	struct pkvm_vcpu *pkvm_vcpu = to_pkvm_vcpu(vcpu);
	struct shadow_ept_desc *sept_desc;

	shadow_vcpu->shadow_vcpu_handle =
		to_shadow_vcpu_handle(vm_handle, pkvm_vcpu->vcpu_idx);
	shadow_vcpu->vm = kvm_to_shadow(vcpu->kvm);

	shadow_vcpu->vmcs02 = to_vmx(vcpu)->vmcs01.vmcs;
	shadow_vcpu->vmcs12_pa = __pkvm_pa(to_vmx(vcpu)->vmcs01.vmcs);
	add_shadow_vcpu_vmcs12_map(shadow_vcpu);

	sept_desc = &shadow_vcpu->vm->sept_desc;
	vcpu->arch.root_mmu.root_role.level = sept_desc->sept.level;
	vcpu->arch.root_mmu.root.hpa = sept_desc->sept.root_pa;
	vcpu->arch.mmu = &vcpu->arch.root_mmu;

	if (!shadow_vm_is_protected(shadow_vcpu->vm))
		shadow_vcpu->allowed_to_run = true;

	return 0;
}

void pkvm_teardown_shadow_vcpu(struct kvm_vcpu *vcpu)
{
	struct shadow_vcpu_state *shadow_vcpu = kvm_vcpu_to_shadow(vcpu);

	remove_shadow_vcpu_vmcs12_map(shadow_vcpu);
	shadow_vcpu->vm = NULL;
	shadow_vcpu->shadow_vcpu_handle = 0;
}
