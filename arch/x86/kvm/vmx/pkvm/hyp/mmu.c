// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/memblock.h>
#include <asm/kvm_pkvm.h>
#include <asm/pkvm_spinlock.h>
#include <mmu.h>
#include <mmu/spte.h>

#include <pkvm.h>
#include "early_alloc.h"
#include "pgtable.h"
#include "mmu.h"
#include "debug.h"

static struct pkvm_pgtable hyp_mmu;

static bool mmu_entry_present(void *ptep)
{
	return pte_present(*(pte_t *)ptep);
}

static bool mmu_entry_huge(void *ptep)
{
	return pte_huge(*(pte_t *)ptep);
}

static void mmu_entry_mkhuge(void *ptep)
{
	pte_t *ptep_ptr = (pte_t *)ptep;

	*ptep_ptr = pte_mkhuge(*ptep_ptr);
}

static unsigned long mmu_entry_to_phys(void *ptep)
{
	return native_pte_val(*(pte_t *)ptep) & PTE_PFN_MASK;
}

static u64 mmu_entry_to_prot(void *ptep)
{
	return (u64)pte_flags(pte_clear_flags(*(pte_t *)ptep, _PAGE_PSE));
}

static int mmu_entry_to_index(unsigned long vaddr, int level)
{
	return SPTE_INDEX(vaddr, level);
}

static bool mmu_entry_is_leaf(void *ptep, int level)
{
	if (level == PG_LEVEL_4K ||
		!mmu_entry_present(ptep) ||
		mmu_entry_huge(ptep))
		return true;

	return false;
}

static int mmu_level_entry_size(int level)
{
	return PAGE_SIZE / PTRS_PER_PTE;
}

static int mmu_level_to_entries(int level)
{
	return PTRS_PER_PTE;
}

static unsigned long mmu_level_to_size(int level)
{
	return page_level_size(level);
}

static void mmu_set_entry(void *ptep, u64 pte)
{
	native_set_pte((pte_t *)ptep, native_make_pte(pte));
}

struct pkvm_pgtable_ops mmu_ops = {
	.pgt_entry_present = mmu_entry_present,
	.pgt_entry_huge = mmu_entry_huge,
	.pgt_entry_mkhuge = mmu_entry_mkhuge,
	.pgt_entry_to_phys = mmu_entry_to_phys,
	.pgt_entry_to_prot = mmu_entry_to_prot,
	.pgt_entry_to_index = mmu_entry_to_index,
	.pgt_entry_is_leaf = mmu_entry_is_leaf,
	.pgt_level_entry_size = mmu_level_entry_size,
	.pgt_level_to_entries = mmu_level_to_entries,
	.pgt_level_to_size = mmu_level_to_size,
	.pgt_set_entry = mmu_set_entry,
};

int pkvm_mmu_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot)
{
	int ret;

	ret = pkvm_pgtable_map(&hyp_mmu, vaddr_start, phys_start, size, pgsz_mask, prot);

	return ret;
}

int pkvm_mmu_unmap(unsigned long vaddr_start, unsigned long phys_start, unsigned long size)
{
	int ret;

	ret = pkvm_pgtable_unmap(&hyp_mmu, vaddr_start, phys_start, size);

	return ret;
}

/* early mmu init before vmemmap ready, use early allocator first */
int pkvm_early_mmu_init(struct pkvm_pgtable_cap *cap,
		void *mmu_pool_base, unsigned long mmu_pool_pages)
{
	pkvm_early_alloc_init(mmu_pool_base, mmu_pool_pages << PAGE_SHIFT);

	return pkvm_pgtable_init(&hyp_mmu, &pkvm_early_alloc_mm_ops, &mmu_ops, cap, true);
}
