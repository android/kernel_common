// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <kvm/arm_hypercalls.h>

#include <nvhe/mem_protect.h>
#include <nvhe/pkvm.h>
#include <nvhe/pviommu.h>

static bool pkvm_guest_iommu_map(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static bool pkvm_guest_iommu_unmap(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static bool pkvm_guest_iommu_attach_dev(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static bool pkvm_guest_iommu_detach_dev(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static bool pkvm_guest_iommu_alloc_domain(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

static bool pkvm_guest_iommu_free_domain(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return false;
}

bool kvm_handle_pviommu_hvc(struct kvm_vcpu *vcpu, u64 *exit_code)
{
	u64 iommu_op = smccc_get_arg1(vcpu);
	struct pkvm_hyp_vcpu *hyp_vcpu = container_of(vcpu, struct pkvm_hyp_vcpu, vcpu);
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	/*
	 * Eagerly fill the vm iommu pool to avoid deadlocks from donation path while
	 * doing IOMMU operations.
	 */
	refill_hyp_pool(&vm->iommu_pool, &hyp_vcpu->host_vcpu->arch.iommu_mc);
	switch (iommu_op) {
	case KVM_PVIOMMU_OP_ALLOC_DOMAIN:
		return pkvm_guest_iommu_alloc_domain(hyp_vcpu);
	case KVM_PVIOMMU_OP_FREE_DOMAIN:
		return pkvm_guest_iommu_free_domain(hyp_vcpu);
	case KVM_PVIOMMU_OP_ATTACH_DEV:
		return pkvm_guest_iommu_attach_dev(hyp_vcpu);
	case KVM_PVIOMMU_OP_DETACH_DEV:
		return pkvm_guest_iommu_detach_dev(hyp_vcpu);
	case KVM_PVIOMMU_OP_MAP_PAGES:
		return pkvm_guest_iommu_map(hyp_vcpu);
	case KVM_PVIOMMU_OP_UNMAP_PAGES:
		return pkvm_guest_iommu_unmap(hyp_vcpu);
	}

	smccc_set_retval(vcpu, SMCCC_RET_NOT_SUPPORTED, 0, 0, 0);
	return true;
}
