// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 - Google LLC
 * Author: Quentin Perret <qperret@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/memblock.h>
#include <linux/sort.h>

#include <asm/kvm_pkvm.h>
#include "../pkvm.h"

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

struct hyp_shared_pfn {
	u64 pfn;
	int count;
	struct rb_node node;
};

static DEFINE_MUTEX(hyp_shared_pfns_lock);
static struct rb_root hyp_shared_pfns = RB_ROOT;

static struct hyp_shared_pfn *find_shared_pfn(u64 pfn, struct rb_node ***node,
					      struct rb_node **parent)
{
	struct hyp_shared_pfn *this;

	*node = &hyp_shared_pfns.rb_node;
	*parent = NULL;
	while (**node) {
		this = container_of(**node, struct hyp_shared_pfn, node);
		*parent = **node;
		if (this->pfn < pfn)
			*node = &((**node)->rb_left);
		else if (this->pfn > pfn)
			*node = &((**node)->rb_right);
		else
			return this;
	}

	return NULL;
}

static int share_pfn_hyp(u64 pfn)
{
	struct rb_node **node, *parent;
	struct hyp_shared_pfn *this;
	int ret;

	guard(mutex)(&hyp_shared_pfns_lock);

	this = find_shared_pfn(pfn, &node, &parent);
	if (this) {
		this->count++;
		return 0;
	}

	this = kzalloc(sizeof(*this), GFP_KERNEL);
	if (!this)
		return -ENOMEM;

	ret = kvm_call_pkvm(host_share_hyp, pfn, 1);
	if (ret) {
		kfree(this);
		return ret;
	}

	this->pfn = pfn;
	this->count = 1;
	rb_link_node(&this->node, parent, node);
	rb_insert_color(&this->node, &hyp_shared_pfns);

	return 0;
}

static int unshare_pfn_hyp(u64 pfn)
{
	struct rb_node **node, *parent;
	struct hyp_shared_pfn *this;
	int ret;

	guard(mutex)(&hyp_shared_pfns_lock);

	this = find_shared_pfn(pfn, &node, &parent);
	if (WARN_ON(!this))
		return -ENOENT;

	this->count--;
	if (this->count)
		return 0;

	ret = kvm_call_pkvm(host_unshare_hyp, pfn, 1);
	if (ret) {
		/* Revert back the counter. */
		this->count++;
		return ret;
	}

	rb_erase(&this->node, &hyp_shared_pfns);
	kfree(this);

	return 0;
}

int kvm_share_hyp(void *from, void *to)
{
	phys_addr_t start, end, cur;
	u64 pfn;
	int ret;

	/*
	 * The share PV interface maps things in the 'fixed-offset' region of
	 * the hyp VA space, so we can only share physically contiguous
	 * data-structures for now.
	 */
	if (is_vmalloc_or_module_addr(from) || is_vmalloc_or_module_addr(to))
		return -EINVAL;

	start = ALIGN_DOWN(__pa(from), PAGE_SIZE);
	end = ALIGN(__pa(to), PAGE_SIZE);
	for (cur = start; cur < end; cur += PAGE_SIZE) {
		pfn = __phys_to_pfn(cur);
		ret = share_pfn_hyp(pfn);
		if (ret)
			return ret;
	}

	return 0;
}

void kvm_unshare_hyp(void *from, void *to)
{
	phys_addr_t start, end, cur;
	u64 pfn;

	if (!from || !to)
		return;

	start = ALIGN_DOWN(__pa(from), PAGE_SIZE);
	end = ALIGN(__pa(to), PAGE_SIZE);
	for (cur = start; cur < end; cur += PAGE_SIZE) {
		pfn = __phys_to_pfn(cur);
		WARN_ON(unshare_pfn_hyp(pfn));
	}
}
