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
#define EPT_PROT_DEF		VMX_EPT_SUPPRESS_VE_BIT

void host_ept_lock(void);
void host_ept_unlock(void);
int pkvm_host_ept_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot);
int pkvm_host_ept_unmap(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size);
void pkvm_host_ept_lookup(unsigned long vaddr, unsigned long *pphys,
			  u64 *pprot, int *plevel);
int pkvm_host_ept_init(struct pkvm_pgtable_cap *cap, void *ept_pool_base,
		unsigned long ept_pool_pages);
int handle_host_ept_violation(struct kvm_vcpu *vcpu);
void pkvm_flush_host_ept(void);
int pkvm_shadow_ept_pool_init(void *ept_pool_base, unsigned long ept_pool_pages);

int pkvm_pgstate_pgt_init(struct pkvm_shadow_vm *vm);
void pkvm_pgstate_pgt_deinit(struct pkvm_shadow_vm *vm);

const struct pkvm_mm_ops *pkvm_shadow_sl_iommu_pgt_get_mm_ops(bool coherent);
void pkvm_shadow_sl_iommu_pgt_update_coherency(struct pkvm_pgtable *pgt, bool coherent);

bool is_pgt_ops_ept(struct pkvm_pgtable *pgt);

extern const struct pkvm_pgtable_ops ept_ops;
extern struct hyp_pool shadow_pgt_pool;

#endif
