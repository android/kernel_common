// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <asm/string.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>

static unsigned long base;
static unsigned long end;
static unsigned long cur;

static pkvm_spinlock_t early_lock = __PKVM_SPINLOCK_UNLOCKED;

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

void pkvm_early_alloc_init(void *virt, unsigned long size)
{
	base = cur = (unsigned long)virt;
	end = base + size;
}
