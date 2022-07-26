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

int pkvm_host_ept_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot);
int pkvm_host_ept_unmap(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size);
int pkvm_host_ept_init(struct pkvm_pgtable_cap *cap, void *ept_pool_base,
		unsigned long ept_pool_pages);
int handle_host_ept_violation(unsigned long gpa);
int pkvm_shadow_ept_pool_init(void *ept_pool_base, unsigned long ept_pool_pages);
int pkvm_shadow_ept_init(struct shadow_ept_desc *desc);
void pkvm_shadow_ept_deinit(struct shadow_ept_desc *desc);

#endif
