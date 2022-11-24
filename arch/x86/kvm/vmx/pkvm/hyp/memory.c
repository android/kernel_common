// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/types.h>
#include <asm/kvm_pkvm.h>

#include "memory.h"

unsigned long __page_base_offset;
unsigned long __symbol_base_offset;

unsigned int hyp_memblock_nr;
struct memblock_region hyp_memory[HYP_MEMBLOCK_REGIONS];

void *pkvm_phys_to_virt(unsigned long phys)
{
	return (void *)__page_base_offset + phys;
}

unsigned long pkvm_virt_to_phys(void *virt)
{
	return (unsigned long)virt - __page_base_offset;
}

unsigned long pkvm_virt_to_symbol_phys(void *virt)
{
	return (unsigned long)virt - __symbol_base_offset;
}

bool find_mem_range(unsigned long addr, struct mem_range *range)
{
	int cur, left = 0, right = hyp_memblock_nr;
	struct memblock_region *reg;
	unsigned long end;

	range->start = 0;
	range->end = ULONG_MAX;

	/* The list of memblock regions is sorted, binary search it */
	while (left < right) {
		cur = (left + right) >> 1;
		reg = &hyp_memory[cur];
		end = reg->base + reg->size;
		if (addr < reg->base) {
			right = cur;
			range->end = reg->base;
		} else if (addr >= end) {
			left = cur + 1;
			range->start = end;
		} else {
			range->start = reg->base;
			range->end = end;
			return true;
		}
	}

	return false;
}

bool mem_range_included(struct mem_range *child, struct mem_range *parent)
{
	return parent->start <= child->start && child->end <= parent->end;
}
