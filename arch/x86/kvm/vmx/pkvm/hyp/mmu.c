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
#include "pkvm_hyp.h"
#include "gfp.h"
#include "early_alloc.h"
#include "pgtable.h"
#include "mmu.h"
#include "debug.h"

static struct hyp_pool mmu_pool;
static struct pkvm_pgtable hyp_mmu;
static pkvm_spinlock_t _hyp_mmu_lock = __PKVM_SPINLOCK_UNLOCKED;

static void *mmu_zalloc_page(void)
{
	return hyp_alloc_pages(&mmu_pool, 0);
}

static void mmu_get_page(void *vaddr)
{
	hyp_get_page(&mmu_pool, vaddr);
}

static void mmu_put_page(void *vaddr)
{
	hyp_put_page(&mmu_pool, vaddr);
}

static void flush_tlb_noop(struct pkvm_pgtable *pgt) { };

static struct pkvm_mm_ops mmu_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = mmu_zalloc_page,
	.get_page = mmu_get_page,
	.put_page = mmu_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = flush_tlb_noop,
};

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

static u64 mmu_level_page_mask(int level)
{
	return (~((1UL << SPTE_LEVEL_SHIFT(level)) - 1));
}

struct pkvm_pgtable_ops mmu_ops = {
	.pgt_entry_present = mmu_entry_present,
	.pgt_entry_mapped = mmu_entry_present,
	.pgt_entry_huge = mmu_entry_huge,
	.pgt_entry_mkhuge = mmu_entry_mkhuge,
	.pgt_entry_to_phys = mmu_entry_to_phys,
	.pgt_entry_to_prot = mmu_entry_to_prot,
	.pgt_entry_to_index = mmu_entry_to_index,
	.pgt_level_page_mask = mmu_level_page_mask,
	.pgt_entry_is_leaf = mmu_entry_is_leaf,
	.pgt_level_entry_size = mmu_level_entry_size,
	.pgt_level_to_entries = mmu_level_to_entries,
	.pgt_level_to_size = mmu_level_to_size,
	.pgt_set_entry = mmu_set_entry,
	.default_prot = MMU_PROT_DEF,
};

static int finalize_host_mappings_walker(struct pkvm_pgtable *mmu,
					 unsigned long vaddr,
					 unsigned long vaddr_end,
					 int level,
					 void *ptep,
					 unsigned long flags,
					 struct pgt_flush_data *flush_data,
					 void *const arg)
{
	struct pkvm_mm_ops *mm_ops = arg;
	struct pkvm_pgtable_ops *pgt_ops = mmu->pgt_ops;

	if (!pgt_ops->pgt_entry_present(ptep))
		return 0;

	/*
	 * Fix-up the refcount for the page-table pages as the early allocator
	 * was unable to access the pkvm_vmemmap and so the buddy allocator has
	 * initialized the refcount to '1'.
	 */
	mm_ops->get_page(ptep);

	return 0;
}

static int fix_pgtable_refcnt(void)
{
	unsigned long size;
	struct pkvm_pgtable_ops *pgt_ops;
	struct pkvm_pgtable_walker walker = {
		.cb 	= finalize_host_mappings_walker,
		.flags 	= PKVM_PGTABLE_WALK_LEAF | PKVM_PGTABLE_WALK_TABLE_POST,
		.arg 	= hyp_mmu.mm_ops,
	};

	pgt_ops = hyp_mmu.pgt_ops;
	/*
	 * Calculate the max address space, then walk the [0, size) address
	 * range to fixup refcount of every used page.
	 */
#ifdef CONFIG_PKVM_INTEL_DEBUG
	/*
	 * only fix vmmemap range for debug mode, now for 64T memory,
	 * could be extended if physical memory is bigger than 64T
	 */
	size = (SZ_64T / PAGE_SIZE) * sizeof(struct hyp_page);
#else
	size = pgt_ops->pgt_level_to_size(hyp_mmu.level + 1);
#endif

	return pgtable_walk(&hyp_mmu, 0, size, true, &walker);
}

int pkvm_mmu_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot)
{
	int ret;

	pkvm_spin_lock(&_hyp_mmu_lock);
	ret = pkvm_pgtable_map(&hyp_mmu, vaddr_start, phys_start, size,
			pgsz_mask, prot, NULL);
	pkvm_spin_unlock(&_hyp_mmu_lock);

	return ret;
}

int pkvm_mmu_unmap(unsigned long vaddr_start, unsigned long size)
{
	int ret;

	pkvm_spin_lock(&_hyp_mmu_lock);
	ret = pkvm_pgtable_unmap(&hyp_mmu, vaddr_start, size, NULL);
	pkvm_spin_unlock(&_hyp_mmu_lock);

	return ret;
}

/* early mmu init before vmemmap ready, use early allocator first */
int pkvm_early_mmu_init(struct pkvm_pgtable_cap *cap,
		void *mmu_pool_base, unsigned long mmu_pool_pages)
{
	pkvm_early_alloc_init(mmu_pool_base, mmu_pool_pages << PAGE_SHIFT);
	pkvm_hyp->mmu = &hyp_mmu;

	return pkvm_pgtable_init(&hyp_mmu, &pkvm_early_alloc_mm_ops, &mmu_ops, cap, true);
}

/* later mmu init after vmemmap ready, switch to buddy allocator */
int pkvm_later_mmu_init(void *mmu_pool_base, unsigned long mmu_pool_pages)
{
	unsigned long reserved_pages, pfn;
	int ret;

	/* Enable buddy allocator */
	pfn = __pkvm_pa(mmu_pool_base) >> PAGE_SHIFT;
	reserved_pages = pkvm_early_alloc_nr_used_pages();
	ret = hyp_pool_init(&mmu_pool, pfn, mmu_pool_pages, reserved_pages);
	if (ret) {
		pkvm_err("fail to init mmu_pool");
		return ret;
	}

	/* The ops should alloc memory from mmu_pool now */
	hyp_mmu.mm_ops = &mmu_mm_ops;

	/*
	 * as we used early alloc mm_ops to create early pgtable mapping for mmu,
	 * the refcount was not maintained at that time, we need fix it by re-walk
	 * the pgtable
	 */
	return fix_pgtable_refcnt();
}

#ifdef CONFIG_PKVM_INTEL_DEBUG
void pkvm_mmu_clone_host(int level, unsigned long start_vaddr)
{
	int i = mmu_entry_to_index(start_vaddr, level);
	u64 *ptep = __va(hyp_mmu.root_pa);
	u64 *host_cr3 = __va(__read_cr3() & PAGE_MASK);

	for (; i < PTRS_PER_PTE; i++)
		ptep[i] = host_cr3[i];

}
#endif
