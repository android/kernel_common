// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <asm/string.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>

#include "pgtable.h"

static unsigned long base;
static unsigned long end;
static unsigned long cur;

static pkvm_spinlock_t early_lock = __PKVM_SPINLOCK_UNLOCKED;

struct pkvm_mm_ops pkvm_early_alloc_mm_ops;

unsigned long pkvm_early_alloc_nr_used_pages(void)
{
	return (cur - base) >> PAGE_SHIFT;
}

void *pkvm_early_alloc_contig(unsigned int nr_pages)
{
	unsigned long size = (nr_pages << PAGE_SHIFT);
	void *ret;

	if (!nr_pages)
		return NULL;

	pkvm_spin_lock(&early_lock);
	if (end - cur < size) {
		pkvm_spin_unlock(&early_lock);
		return NULL;
	}
	ret = (void *)cur;
	cur += size;
	pkvm_spin_unlock(&early_lock);

	memset(ret, 0, size);

	return ret;
}

void *pkvm_early_alloc_page(void)
{
	return pkvm_early_alloc_contig(1);
}

static void pkvm_early_alloc_get_page(void *addr) { }
static void pkvm_early_alloc_put_page(void *addr) { }
static void pkvm_early_flush_tlb(void) { }

static int pkvm_early_page_count(void *vaddr)
{
	return 512;
}

void pkvm_early_alloc_init(void *virt, unsigned long size)
{
	base = cur = (unsigned long)virt;
	end = base + size;

	pkvm_early_alloc_mm_ops.zalloc_page = pkvm_early_alloc_page;
	pkvm_early_alloc_mm_ops.get_page = pkvm_early_alloc_get_page;
	pkvm_early_alloc_mm_ops.put_page = pkvm_early_alloc_put_page;
	pkvm_early_alloc_mm_ops.phys_to_virt = pkvm_phys_to_virt;
	pkvm_early_alloc_mm_ops.virt_to_phys = pkvm_virt_to_phys;
	pkvm_early_alloc_mm_ops.page_count = pkvm_early_page_count;
	pkvm_early_alloc_mm_ops.flush_tlb = pkvm_early_flush_tlb;
}
