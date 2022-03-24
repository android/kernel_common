// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/types.h>
#include <linux/memblock.h>
#include <asm/kvm_pkvm.h>
#include <mmu.h>
#include <mmu/spte.h>

#include <pkvm.h>

#include "gfp.h"
#include "early_alloc.h"
#include "pgtable.h"
#include "ept.h"

static struct hyp_pool host_ept_pool;
static struct pkvm_pgtable host_ept;

static void flush_tlb_noop(void) { };
static void *host_ept_zalloc_page(void)
{
	return hyp_alloc_pages(&host_ept_pool, 0);
}

static void host_ept_get_page(void *vaddr)
{
	hyp_get_page(&host_ept_pool, vaddr);
}

static void host_ept_put_page(void *vaddr)
{
	hyp_put_page(&host_ept_pool, vaddr);
}

/*TODO: add tlb flush support for host ept */
struct pkvm_mm_ops host_ept_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = host_ept_zalloc_page,
	.get_page = host_ept_get_page,
	.put_page = host_ept_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = flush_tlb_noop,
};

static bool ept_entry_present(void *ptep)
{
	u64 val = *(u64 *)ptep;

	return !!(val & VMX_EPT_RWX_MASK);
}

static bool ept_entry_huge(void *ptep)
{
	return is_large_pte(*(u64 *)ptep);
}

static void ept_entry_mkhuge(void *ptep)
{
	*(u64 *)ptep |= PT_PAGE_SIZE_MASK;
}

static unsigned long ept_entry_to_phys(void *ptep)
{
	return *(u64 *)ptep & SPTE_BASE_ADDR_MASK;
}

static u64 ept_entry_to_prot(void *ptep)
{
	u64 prot = *(u64 *)ptep & ~(SPTE_BASE_ADDR_MASK);

	return prot & ~PT_PAGE_SIZE_MASK;
}

static int ept_entry_to_index(unsigned long vaddr, int level)
{
	return SPTE_INDEX(vaddr, level);
}

static bool ept_entry_is_leaf(void *ptep, int level)
{
	if (level == PG_LEVEL_4K ||
		!ept_entry_present(ptep) ||
		ept_entry_huge(ptep))
		return true;

	return false;

}

static int ept_level_entry_size(int level)
{
	return PAGE_SIZE / SPTE_ENT_PER_PAGE;
}

static int ept_level_to_entries(int level)
{
	return SPTE_ENT_PER_PAGE;
}

static unsigned long ept_level_to_size(int level)
{
	return KVM_HPAGE_SIZE(level);
}

static void ept_set_entry(void *sptep, u64 spte)
{
	WRITE_ONCE(*(u64 *)sptep, spte);
}

struct pkvm_pgtable_ops ept_ops = {
	.pgt_entry_present = ept_entry_present,
	.pgt_entry_huge = ept_entry_huge,
	.pgt_entry_mkhuge = ept_entry_mkhuge,
	.pgt_entry_to_phys = ept_entry_to_phys,
	.pgt_entry_to_prot = ept_entry_to_prot,
	.pgt_entry_to_index = ept_entry_to_index,
	.pgt_entry_is_leaf = ept_entry_is_leaf,
	.pgt_level_entry_size = ept_level_entry_size,
	.pgt_level_to_entries = ept_level_to_entries,
	.pgt_level_to_size = ept_level_to_size,
	.pgt_set_entry = ept_set_entry,
};

int pkvm_host_ept_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot)
{
	return pkvm_pgtable_map(&host_ept, vaddr_start, phys_start, size, pgsz_mask, prot);
}

int pkvm_host_ept_unmap(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size)
{
	return pkvm_pgtable_unmap(&host_ept, vaddr_start, phys_start, size);
}

int pkvm_host_ept_init(struct pkvm_pgtable_cap *cap,
		void *ept_pool_base, unsigned long ept_pool_pages)
{
	unsigned long pfn = __pkvm_pa(ept_pool_base) >> PAGE_SHIFT;
	int ret;

	ret = hyp_pool_init(&host_ept_pool, pfn, ept_pool_pages, 0);
	if (ret)
		return ret;

	return pkvm_pgtable_init(&host_ept, &host_ept_mm_ops, &ept_ops, cap, true);
}
