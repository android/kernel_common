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

/*
 * __pkvm_host_donate_hyp() - Donate pages from host to hyp, then host cannot
 * access these donated pages.
 *
 * @hpa:	Start hpa of being donated pages, must be continuous.
 * @size:	The size of memory to be donated.
 *
 * A range of pages [hpa, hpa + size) will be donated from host to hyp. And
 * this will unmap these pages from host ept and set the page owner as hyp_id
 * in the pte in host ept. For hyp mmu, it will do nothing as hyp mmu can
 * access all the memory by default, but modifying host ept is necessary because
 * a page used by pkvm is private and can't be accessed by host.
 */
int __pkvm_host_donate_hyp(u64 hpa, u64 size);

/*
 * __pkvm_hyp_donate_host() - Donate pages from hyp to host, then host can
 * access these pages.
 *
 * @hpa:	Start hpa of being donated pages, must be continuous.
 * @size:	The size of memory to be donated.
 *
 * A range of pages [hpa, hpa + size) will be donated from hyp to host. This
 * will create mapping in host ept for these pages, and nothing to do with hyp
 * mmu. This is paired with __pkvm_host_donate_hyp(), and same as host reclaiming
 * these pages back.
 */
int __pkvm_hyp_donate_host(u64 hpa, u64 size);

/*
 * __pkvm_host_share_guest() - Share pages between host and guest. Host still
 * ownes the page and guest will have temporary access to these pages.
 *
 * @hpa:	Start hpa of being shared pages, must be continuous.
 * @guest_pgt:	The guest ept pagetable.
 * @gpa:	Start gpa that will be used for mapping into the guest ept.
 * @size:	The size of pages to be shared.
 * @prot:	The prot that will be used for creating mapping for guest ept.
 *
 * A range of pages [hpa, hpa + size) in host ept that their page state
 * will be modified from PAGE_OWNED to PAGE_SHARED_OWNED. There will be
 * mapping from gfn to pfn to be created in guest ept. The @prot
 * and PAGE_SHARED_BORROWED will be used to create such mapping.
 */
int __pkvm_host_share_guest(u64 hpa, struct pkvm_pgtable *guest_pgt,
			    u64 gpa, u64 size, u64 prot);

/*
 * __pkvm_host_unshare_guest() - Host unshare pages that have been shared to guest
 * previously. Guest will not be able to access these pages.
 *
 * @hpa:	Start hpa of being shared pages, must be continuous.
 * @guest_pgt:	The guest ept pagetable.
 * @gpa:	Start gpa of shared pages being mapped in guest ept.
 * @size:	The size of pages to be shared.
 *
 * Unmap the range [gfn, gfn + nr_pages) in guest ept pagetable. And change
 * the page state from PAGE_SHARED_BORROWED to PAGE_OWNED in the host ept.
 */
int __pkvm_host_unshare_guest(u64 hpa, struct pkvm_pgtable *guest_pgt,
			      u64 gpa, u64 size);

/*
 * __pkvm_host_donate_guest() - Host donate pages to guest. Then host can't
 * access these pages and guest can access.
 *
 * @hpa:	Start hpa of being donated pages, must be continues.
 * @guest_pgt:	The guest ept pagetable.
 * @gpa:	Start gpa of donated pages that will be mapped in guest ept.
 * @size:	The size of pages to being donated.
 * @prot:	The prot that will be used for creating mapping in guest ept.
 *
 * A range of pages [hpa, hpa + size) will be donated from host to guest. And
 * this will unmap these pages from host ept and set the page owner as guest_id
 * in the pte in host ept. The guest_id is equal to the vm's shadow_handle+1. In
 * the same time, the mapping gpa -> hpa with @size will be created in guest ept
 * with @prot.
 */
int __pkvm_host_donate_guest(u64 hpa, struct pkvm_pgtable *guest_pgt,
			     u64 gpa, u64 size, u64 prot);
#endif
