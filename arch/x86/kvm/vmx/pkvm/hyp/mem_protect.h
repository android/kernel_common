// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_MEM_PROTECT_H__
#define __PKVM_MEM_PROTECT_H__

/*
 * enum pkvm_pgtable_prot - The ignored bits in page-table.
 * pkvm will use these ignored bits as software bits to
 * identify the page status.
 */
enum pkvm_pgtable_prot {
	PKVM_PGTABLE_PROT_SW0		= BIT(56),
	PKVM_PGTABLE_PROT_SW1		= BIT(57),
};

/*
 * Using the ignored bits in page-table as SW bits.
 * SW bits 0-1 are used to track the memory ownership state of each page:
 *   00: The page has no mapping in page table (also invalid pte). And under
 *   this page state, host ept is using the pte ignored bits to record owner_id.
 *   01: The page is owned exclusively by the page-table owner.
 *   10: The page is owned by the page-table owner, but is shared
 *   	with another entity.
 *   11: The page is shared with, but not owned by the page-table owner.
 */
enum pkvm_page_state {
	PKVM_NOPAGE			= 0ULL,
	PKVM_PAGE_OWNED			= PKVM_PGTABLE_PROT_SW0,
	PKVM_PAGE_SHARED_OWNED		= PKVM_PGTABLE_PROT_SW1,
	PKVM_PAGE_SHARED_BORROWED	= PKVM_PGTABLE_PROT_SW0 |
					  PKVM_PGTABLE_PROT_SW1,
};

#define PKVM_PAGE_STATE_PROT_MASK	(PKVM_PGTABLE_PROT_SW0 | PKVM_PGTABLE_PROT_SW1)
/* use 20 bits[12~31] - not conflict w/ low 12 bits pte prot */
#define PKVM_INVALID_PTE_OWNER_MASK	GENMASK(31, 12)

static inline u64 pkvm_mkstate(u64 prot, enum pkvm_page_state state)
{
	return (prot & ~PKVM_PAGE_STATE_PROT_MASK) | state;
}

static inline enum pkvm_page_state pkvm_getstate(u64 pte)
{
	return pte & PKVM_PAGE_STATE_PROT_MASK;
}

typedef u32 pkvm_id;

#define OWNER_ID_HYP	0UL
#define OWNER_ID_HOST	1UL
#define OWNER_ID_INV	(~(u32)0UL)

#endif
