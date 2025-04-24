// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <kvm/arm_hypercalls.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/pkvm.h>
#include <nvhe/pviommu.h>
#include <nvhe/pviommu-host.h>

static DEFINE_HYP_SPINLOCK(pviommu_guest_domain_lock);

#define KVM_IOMMU_MAX_GUEST_DOMAINS		(KVM_IOMMU_MAX_DOMAINS >> 1)
static unsigned long guest_domains[KVM_IOMMU_MAX_GUEST_DOMAINS / BITS_PER_LONG];

/*
 * Guests doens't have separate domain space as the host, but they share the upper half
 * of the domain ids, so they would ask for a domain and get a domain id as a return.
 * This is a rare operation for guests, so bruteforcing the domain space should be fine
 * for now, however we can improve this by having a hint for last allocated domain_id or
 * use a pseudo-random number.
 */
static int pkvm_guest_iommu_alloc_id(void)
{
	int i;

	for (i = 0 ; i < ARRAY_SIZE(guest_domains) ; ++i) {
		if (guest_domains[i] != ~0UL)
			return ffz(guest_domains[i]) + i * BITS_PER_LONG +
			       (KVM_IOMMU_MAX_DOMAINS >> 1);
	}

	return -EBUSY;
}

static void pkvm_guest_iommu_free_id(int domain_id)
{
	domain_id -= (KVM_IOMMU_MAX_DOMAINS >> 1);
	if (WARN_ON(domain_id < 0) || (domain_id >= KVM_IOMMU_MAX_GUEST_DOMAINS))
		return;

	guest_domains[domain_id / BITS_PER_LONG] &= ~(1UL << (domain_id % BITS_PER_LONG));
}

/*
 * check if vcpu has requested memory before
 */
static bool __need_req(struct kvm_vcpu *vcpu)
{
	struct kvm_hyp_req *hyp_req = vcpu->arch.hyp_reqs;

	return hyp_req->type != KVM_HYP_LAST_REQ;
}

static void pkvm_pviommu_hyp_req(u64 *exit_code)
{
	write_sysreg_el2(read_sysreg_el2(SYS_ELR) - 4, SYS_ELR);
	*exit_code = ARM_EXCEPTION_HYP_REQ;
}

static bool pkvm_guest_iommu_attach_dev(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code)
{
	int ret;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 iommu_id = smccc_get_arg2(vcpu);
	u64 sid = smccc_get_arg3(vcpu);
	u64 pasid = smccc_get_arg4(vcpu);
	u64 domain_id = smccc_get_arg5(vcpu);
	u64 pasid_bits = smccc_get_arg6(vcpu);
	struct pviommu_route route;
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	ret = pkvm_pviommu_get_route(vm, iommu_id, sid, &route);
	if (ret)
		goto out_ret;
	iommu_id = route.iommu;
	sid = route.sid;

	ret = kvm_iommu_attach_dev(iommu_id, domain_id, sid, pasid, pasid_bits);
	if (ret == -ENOMEM) {
		/*
		 * The driver will request memory when returning -ENOMEM, so go back to host to
		 * fulfill the request and repeat the HVC.
		 */
		pkvm_pviommu_hyp_req(exit_code);
		return false;
	}

out_ret:
	smccc_set_retval(vcpu, ret ?  SMCCC_RET_INVALID_PARAMETER : SMCCC_RET_SUCCESS,
			 0, 0, 0);
	return true;
}

static bool pkvm_guest_iommu_detach_dev(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	int ret;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 iommu_id = smccc_get_arg2(vcpu);
	u64 sid = smccc_get_arg3(vcpu);
	u64 pasid = smccc_get_arg4(vcpu);
	u64 domain_id = smccc_get_arg5(vcpu);
	struct pviommu_route route;
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	/* MBZ */
	if (smccc_get_arg6(vcpu)) {
		ret = -EINVAL;
		goto out_ret;
	}

	ret = pkvm_pviommu_get_route(vm, iommu_id, sid, &route);
	if (ret)
		goto out_ret;
	iommu_id = route.iommu;
	sid = route.sid;

	ret = kvm_iommu_detach_dev(iommu_id, domain_id, sid, pasid);

out_ret:
	smccc_set_retval(vcpu, ret ?  SMCCC_RET_INVALID_PARAMETER : SMCCC_RET_SUCCESS,
			 0, 0, 0);
	return true;
}

static bool pkvm_guest_iommu_alloc_domain(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code)
{
	int ret;
	int domain_id = 0;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;

	/* MBZ */
	if (smccc_get_arg2(vcpu) || smccc_get_arg3(vcpu) || smccc_get_arg4(vcpu) ||
	    smccc_get_arg5(vcpu) || smccc_get_arg6(vcpu))
		goto out_inval;

	hyp_spin_lock(&pviommu_guest_domain_lock);
	domain_id = pkvm_guest_iommu_alloc_id();
	if (domain_id < 0)
		goto out_inval;

	ret = kvm_iommu_alloc_domain(domain_id, KVM_IOMMU_DOMAIN_ANY_TYPE);
	if (ret == -ENOMEM) {
		pkvm_guest_iommu_free_id(domain_id);
		hyp_spin_unlock(&pviommu_guest_domain_lock);
		pkvm_pviommu_hyp_req(exit_code);
		return false;
	} else if (ret) {
		pkvm_guest_iommu_free_id(domain_id);
		goto out_inval;
	}

	hyp_spin_unlock(&pviommu_guest_domain_lock);
	smccc_set_retval(vcpu, SMCCC_RET_SUCCESS, domain_id, 0, 0);
	return true;

out_inval:
	hyp_spin_unlock(&pviommu_guest_domain_lock);
	smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
	return true;
}

static bool pkvm_guest_iommu_free_domain(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	int ret;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 domain_id = smccc_get_arg2(vcpu);

	if (smccc_get_arg3(vcpu) || smccc_get_arg4(vcpu) || smccc_get_arg5(vcpu) ||
	    smccc_get_arg6(vcpu)) {
		ret = -EINVAL;
		goto out_ret;
	}

	hyp_spin_lock(&pviommu_guest_domain_lock);
	ret = kvm_iommu_free_domain(domain_id);
	if (!ret)
		pkvm_guest_iommu_free_id(domain_id);
	hyp_spin_unlock(&pviommu_guest_domain_lock);

out_ret:
	smccc_set_retval(vcpu, ret ?  SMCCC_RET_INVALID_PARAMETER : SMCCC_RET_SUCCESS,
			 0, 0, 0);
	return true;
}

static int __smccc_prot_linux(u64 prot)
{
	int iommu_prot = 0;

	if (prot & ARM_SMCCC_KVM_PVIOMMU_READ)
		iommu_prot |= IOMMU_READ;
	if (prot & ARM_SMCCC_KVM_PVIOMMU_WRITE)
		iommu_prot |= IOMMU_WRITE;
	if (prot & ARM_SMCCC_KVM_PVIOMMU_CACHE)
		iommu_prot |= IOMMU_CACHE;
	if (prot & ARM_SMCCC_KVM_PVIOMMU_NOEXEC)
		iommu_prot |= IOMMU_NOEXEC;
	if (prot & ARM_SMCCC_KVM_PVIOMMU_MMIO)
		iommu_prot |= IOMMU_MMIO;
	if (prot & ARM_SMCCC_KVM_PVIOMMU_PRIV)
		iommu_prot |= IOMMU_PRIV;

	return iommu_prot;
}

static bool pkvm_guest_iommu_map(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code)
{
	size_t mapped, total_mapped = 0;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 domain = smccc_get_arg2(vcpu);
	u64 iova = smccc_get_arg3(vcpu);
	u64 ipa = smccc_get_arg4(vcpu);
	u64 size = smccc_get_arg5(vcpu);
	u64 prot = smccc_get_arg6(vcpu);
	u64 paddr;
	int ret;
	s8 level;
	u64 smccc_ret = SMCCC_RET_SUCCESS;

	if (!IS_ALIGNED(size, PAGE_SIZE) ||
	    !IS_ALIGNED(ipa, PAGE_SIZE) ||
	    !IS_ALIGNED(iova, PAGE_SIZE)) {
		smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
		return true;
	}

	while (size) {
		/*
		 * We need to get the PA and atomically use the page temporarily to avoid
		 * racing with relinquish.
		 */
		ret = pkvm_get_guest_pa_request_use_dma(hyp_vcpu, ipa, size,
							&paddr, &level);
		if (ret == -ENOENT) {
			/*
			 * Pages are not mapped and a request was created, updated the guest
			 * state and go back to host
			 */
			goto out_host_request;
		} else if (ret) {
			smccc_ret = SMCCC_RET_INVALID_PARAMETER;
			break;
		}

		mapped = kvm_iommu_map_pages(domain, iova, paddr,
					     PAGE_SIZE, min(size, kvm_granule_size(level)) / PAGE_SIZE,
					     __smccc_prot_linux(prot));
		WARN_ON(__pkvm_unuse_dma(paddr, kvm_granule_size(level), hyp_vcpu));
		if (!mapped) {
			if (!__need_req(vcpu)) {
				smccc_ret = SMCCC_RET_INVALID_PARAMETER;
				break;
			}
			/*
			 * Return back to the host with a request to fill the memcache,
			 * and also update the guest state with what was mapped, so the
			 * next time the vcpu runs it can check that not all requested
			 * memory was mapped, and it would repeat the HVC with the rest
			 * of the range.
			 */
			goto out_host_request;
		}

		ipa += mapped;
		iova += mapped;
		total_mapped += mapped;
		size -= mapped;
	}

	smccc_set_retval(vcpu, smccc_ret, total_mapped, 0, 0);
	return true;
out_host_request:
	*exit_code = ARM_EXCEPTION_HYP_REQ;
	smccc_set_retval(vcpu, SMCCC_RET_SUCCESS, total_mapped, 0, 0);
	return false;
}

static bool pkvm_guest_iommu_unmap(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 domain = smccc_get_arg2(vcpu);
	u64 iova = smccc_get_arg3(vcpu);
	u64 size = smccc_get_arg4(vcpu);
	size_t unmapped;
	unsigned long ret = SMCCC_RET_SUCCESS;

	if (!IS_ALIGNED(size, PAGE_SIZE) ||
	    !IS_ALIGNED(iova, PAGE_SIZE) ||
	    smccc_get_arg5(vcpu) ||
	    smccc_get_arg6(vcpu)) {
		smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
		return true;
	}

	unmapped = kvm_iommu_unmap_pages(domain, iova, PAGE_SIZE, size / PAGE_SIZE);
	if (unmapped < size) {
		if (!__need_req(vcpu)) {
			ret = SMCCC_RET_INVALID_PARAMETER;
		} else {
			/* See comment in pkvm_guest_iommu_map(). */
			*exit_code = ARM_EXCEPTION_HYP_REQ;
			smccc_set_retval(vcpu, SMCCC_RET_SUCCESS, unmapped, 0, 0);
			return false;
		}
	}

	smccc_set_retval(vcpu, ret, unmapped, 0, 0);
	return true;
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
		return pkvm_guest_iommu_alloc_domain(hyp_vcpu, exit_code);
	case KVM_PVIOMMU_OP_FREE_DOMAIN:
		return pkvm_guest_iommu_free_domain(hyp_vcpu);
	case KVM_PVIOMMU_OP_ATTACH_DEV:
		return pkvm_guest_iommu_attach_dev(hyp_vcpu, exit_code);
	case KVM_PVIOMMU_OP_DETACH_DEV:
		return pkvm_guest_iommu_detach_dev(hyp_vcpu);
	case KVM_PVIOMMU_OP_MAP_PAGES:
		return pkvm_guest_iommu_map(hyp_vcpu, exit_code);
	case KVM_PVIOMMU_OP_UNMAP_PAGES:
		return pkvm_guest_iommu_unmap(hyp_vcpu, exit_code);
	}

	smccc_set_retval(vcpu, SMCCC_RET_NOT_SUPPORTED, 0, 0, 0);
	return true;
}
