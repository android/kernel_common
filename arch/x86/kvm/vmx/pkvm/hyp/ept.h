// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_EPT_H
#define __PKVM_EPT_H

#include "pkvm_hyp.h"

#define HOST_EPT_DEF_MEM_PROT   (VMX_EPT_RWX_MASK |				\
				(MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT))
#define HOST_EPT_DEF_MMIO_PROT	(VMX_EPT_RWX_MASK |				\
				(MTRR_TYPE_UNCACHABLE << VMX_EPT_MT_EPTE_SHIFT))
#define EPT_PROT_MASK		(VMX_EPT_RWX_MASK | VMX_EPT_MT_MASK | VMX_EPT_IPAT_BIT)
#define EPT_PROT_DEF		SUPPRESS_VE

#define SHADOW_EPT_MMIO_ENTRY	0

enum sept_handle_ret {
	PKVM_NOT_HANDLED,
	PKVM_HANDLED,
	PKVM_INJECT_EPT_MISC,
};

void host_ept_lock(void);
void host_ept_unlock(void);
int pkvm_host_ept_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot);
int pkvm_host_ept_unmap(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size);
int pkvm_host_ept_init(struct pkvm_pgtable_cap *cap, void *ept_pool_base,
		unsigned long ept_pool_pages);
int handle_host_ept_violation(unsigned long gpa);
void pkvm_flush_host_ept(void);
int pkvm_shadow_ept_pool_init(void *ept_pool_base, unsigned long ept_pool_pages);
int pkvm_shadow_ept_init(struct shadow_ept_desc *desc);
void pkvm_shadow_ept_deinit(struct shadow_ept_desc *desc);
void pkvm_guest_ept_init(struct shadow_vcpu_state *shadow_vcpu, u64 guest_eptp);
void pkvm_guest_ept_deinit(struct shadow_vcpu_state *shadow_vcpu);
enum sept_handle_ret
pkvm_handle_shadow_ept_violation(struct shadow_vcpu_state *shadow_vcpu, u64 l2_gpa, u64 exit_quali);
void pkvm_invalidate_shadow_ept(struct shadow_ept_desc *desc);
void pkvm_invalidate_shadow_ept_with_range(struct shadow_ept_desc *desc,
					   unsigned long vaddr, unsigned long size);
void pkvm_flush_shadow_ept(struct shadow_ept_desc *desc);
void pkvm_shadow_clear_suppress_ve(struct kvm_vcpu *vcpu, unsigned long gfn);

int pkvm_pgstate_pgt_init(struct pkvm_shadow_vm *vm);
void pkvm_pgstate_pgt_deinit(struct pkvm_shadow_vm *vm);

struct pkvm_mm_ops *pkvm_shadow_sl_iommu_pgt_get_mm_ops(bool coherent);
void pkvm_shadow_sl_iommu_pgt_update_coherency(struct pkvm_pgtable *pgt, bool coherent);

bool is_pgt_ops_ept(struct pkvm_pgtable *pgt);

static inline bool is_valid_eptp(u64 eptp)
{
	if (!eptp || (eptp == INVALID_GPA))
		return false;

	/* TODO: other bits check */
	return true;
}

extern struct pkvm_pgtable_ops ept_ops;

#endif
