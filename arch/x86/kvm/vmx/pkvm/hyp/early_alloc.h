// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_EARLY_ALLOC_H
#define __PKVM_EARLY_ALLOC_H

unsigned long pkvm_early_alloc_nr_used_pages(void);
void *pkvm_early_alloc_contig(unsigned int nr_pages);
void *pkvm_early_alloc_page(void);
void pkvm_early_alloc_init(void *virt, unsigned long size);

extern struct pkvm_mm_ops pkvm_early_alloc_mm_ops;

#endif
