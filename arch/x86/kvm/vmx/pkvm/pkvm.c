// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 - Google LLC
 * Author: Quentin Perret <qperret@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/memblock.h>
#include <linux/sort.h>

#include <asm/kvm_pkvm.h>

static struct memblock_region *_hyp_memory = pkvm_sym(hyp_memory);
static unsigned int *hyp_memblock_nr_ptr = &pkvm_sym(hyp_memblock_nr);

phys_addr_t hyp_mem_base;
phys_addr_t hyp_mem_size;

static int cmp_hyp_memblock(const void *p1, const void *p2)
{
	const struct memblock_region *r1 = p1;
	const struct memblock_region *r2 = p2;

	return r1->base < r2->base ? -1 : (r1->base > r2->base);
}

static void __init sort_memblock_regions(void)
{
	sort(_hyp_memory,
	     *hyp_memblock_nr_ptr,
	     sizeof(struct memblock_region),
	     cmp_hyp_memblock,
	     NULL);
}

static int __init register_memblock_regions(void)
{
	struct memblock_region *reg;

	for_each_mem_region(reg) {
		if (*hyp_memblock_nr_ptr >= HYP_MEMBLOCK_REGIONS)
			return -ENOMEM;

		_hyp_memory[*hyp_memblock_nr_ptr] = *reg;
		(*hyp_memblock_nr_ptr)++;
	}
	sort_memblock_regions();

	return 0;
}

void __init kvm_hyp_reserve(void)
{
	int ret;

	if (!enable_pkvm)
		return;

	if (hyp_pre_reserve_check() < 0)
		return;

	ret = register_memblock_regions();
	if (ret) {
		*hyp_memblock_nr_ptr = 0;
		kvm_err("Failed to register hyp memblocks: %d\n", ret);
		return;
	}

	/*
	 * Try to allocate a PMD-aligned region to reduce TLB pressure once
	 * this is unmapped from the host stage-2, and fallback to PAGE_SIZE.
	 */
	hyp_mem_size = hyp_total_reserve_pages() << PAGE_SHIFT;
	hyp_mem_base = memblock_phys_alloc(ALIGN(hyp_mem_size, PMD_SIZE),
					   PMD_SIZE);
	if (!hyp_mem_base)
		hyp_mem_base = memblock_phys_alloc(hyp_mem_size, PAGE_SIZE);
	else
		hyp_mem_size = ALIGN(hyp_mem_size, PMD_SIZE);

	if (!hyp_mem_base) {
		kvm_err("Failed to reserve hyp memory\n");
		return;
	}

	kvm_info("Reserved %lld MiB at 0x%llx\n", hyp_mem_size >> 20,
		 hyp_mem_base);
}

static void pkvm_mc_free_fn(void *addr, void *flags)
{
	if ((unsigned long)flags & PKVM_MC_ACCOUNT_PGTABLE_PAGES)
		kvm_account_pgtable_pages(addr, -1);

	free_page((unsigned long)addr);
}

static void *kvm_host_va(phys_addr_t phys)
{
	return __va(phys);
}

void free_pkvm_memcache(struct pkvm_memcache *mc)
{
	__free_pkvm_memcache(mc, pkvm_mc_free_fn,
			     kvm_host_va, (void *)mc->flags);
}

static void *pkvm_mc_alloc_fn(void *flags)
{
	unsigned long __flags = (unsigned long)flags;
	void *addr;

	addr = (void *)__get_free_page(GFP_KERNEL_ACCOUNT);

	if (addr && __flags & PKVM_MC_ACCOUNT_PGTABLE_PAGES)
		kvm_account_pgtable_pages(addr, 1);

	return addr;
}

static phys_addr_t host_pa(void *addr)
{
	return __pa(addr);
}

int topup_pkvm_memcache(struct pkvm_memcache *mc, unsigned long min_pages)
{
	unsigned long flags = mc->flags;

	return __topup_pkvm_memcache(mc, min_pages, pkvm_mc_alloc_fn,
				     host_pa, (void *)flags);
}
