/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GCMA_H__
#define __GCMA_H__

#include <linux/types.h>

#ifdef CONFIG_GCMA
extern void gcma_alloc_range(unsigned long start_pfn, unsigned long end_pfn);
extern void gcma_free_range(unsigned long start_pfn, unsigned long end_pfn);
extern int register_gcma_area(const char *name, phys_addr_t base,
				phys_addr_t size);
#else
static inline void gcma_alloc_range(unsigned long start_pfn,
				    unsigned long end_pfn) {}
static inline void gcma_free_range(unsigned long start_pfn,
				   unsigned long end_pfn) {}
static inline int register_gcma_area(const char *name, phys_addr_t base,
				     phys_addr_t size) { return -EINVAL; }
#endif

#endif
