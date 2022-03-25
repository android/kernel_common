// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_MMU_H_
#define _PKVM_MMU_H_

int pkvm_mmu_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot);

int pkvm_mmu_unmap(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size);

int pkvm_early_mmu_init(struct pkvm_pgtable_cap *cap,
		void *mmu_pool_base, unsigned long mmu_pool_pages);

int pkvm_later_mmu_init(void *mmu_pool_base, unsigned long mmu_pool_pages);

#ifdef CONFIG_PKVM_INTEL_DEBUG
void pkvm_mmu_clone_host(int level, unsigned long start_vaddr);
#else
static inline void pkvm_mmu_clone_host(int level, unsigned long start_vaddr) {}
#endif

#endif
