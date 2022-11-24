// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_MEMORY_H_
#define _PKVM_MEMORY_H_

#include <asm/kvm_pkvm.h>

#define INVALID_ADDR (~0UL)

unsigned long pkvm_virt_to_symbol_phys(void *virt);
#define __pkvm_pa_symbol(x) pkvm_virt_to_symbol_phys((void *)x)

struct mem_range {
	unsigned long start;
	unsigned long end;
};

bool find_mem_range(unsigned long addr, struct mem_range *range);
bool mem_range_included(struct mem_range *child, struct mem_range *parent);

#endif
