// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_PGTABLE_H_
#define _PKVM_PGTABLE_H_

#include <linux/types.h>
#include <asm/pgtable_types.h>

#define SUPPRESS_VE	BIT(63)

struct pkvm_mm_ops {
	void *(*phys_to_virt)(unsigned long phys);
	unsigned long (*virt_to_phys)(void *vaddr);
	void *(*zalloc_page)(void);
	int (*page_count)(void *vaddr);
	void (*get_page)(void *vaddr);
	void (*put_page)(void *vaddr);
	void (*flush_tlb)(struct pkvm_pgtable *pgt);
	void (*flush_cache)(void *vaddr, unsigned int size);
};

struct pkvm_pgtable_ops {
	bool (*pgt_entry_present)(void *pte);
	bool (*pgt_entry_mapped)(void *pte);
	bool (*pgt_entry_huge)(void *pte);
	void (*pgt_entry_mkhuge)(void *ptep);
	unsigned long (*pgt_entry_to_phys)(void *pte);
	u64 (*pgt_entry_to_prot)(void *pte);
	int (*pgt_entry_to_index)(unsigned long vaddr, int level);
	u64 (*pgt_level_page_mask)(int level);
	bool (*pgt_entry_is_leaf)(void *ptep, int level);
	int (*pgt_level_entry_size)(int level);
	int (*pgt_level_to_entries)(int level);
	unsigned long (*pgt_level_to_size)(int level);
	void (*pgt_set_entry)(void *ptep, u64 val);
	u64 default_prot;
};

struct pkvm_pgtable {
	unsigned long root_pa;
	int level;
	int allowed_pgsz;
	u64 table_prot;
	struct pkvm_mm_ops *mm_ops;
	struct pkvm_pgtable_ops *pgt_ops;
};

struct pgt_flush_data {
	bool flushtlb;
	struct list_head free_list;
};

typedef int (*pgtable_visit_fn_t)(struct pkvm_pgtable *pgt, unsigned long vaddr,
				  unsigned long vaddr_end, int level, void *ptep,
				  unsigned long flags, struct pgt_flush_data *flush_data,
				  void *const arg);

typedef int (*pgtable_leaf_ov_fn_t)(struct pkvm_pgtable *pgt, unsigned long vaddr,
				    int level, void *ptep, struct pgt_flush_data *flush_data,
				    void *data);

struct pkvm_pgtable_map_data {
	unsigned long phys;
	u64 annotation;
	u64 prot;
	int pgsz_mask;

	/*
	 * extra override helper ops:
	 * - map_leaf_override():  override the final page entry map function
	 *   		  	   for pkvm_pgtable_map()
	 */
	pgtable_leaf_ov_fn_t map_leaf_override;
};

struct pkvm_pgtable_unmap_data {
	unsigned long phys;

	/*
	 * extra override helper ops:
	 * - unmap_leaf_override(): override the final page entry map function
	 *   for pkvm_pgtable_unmap()
	 */
	pgtable_leaf_ov_fn_t unmap_leaf_override;

	bool split_huge_page;
};

struct pkvm_pgtable_free_data {
	/*
	 * extra override helper ops:
	 * - free_leaf_override(): override the final page entry free function
	 *   		  	   for pkvm_pgtable_destroy()
	 */
	pgtable_leaf_ov_fn_t free_leaf_override;
};

struct pkvm_pgtable_sync_data {
	struct pkvm_pgtable *dest_pgt;
	u64 *prot_override;

	pgtable_leaf_ov_fn_t map_leaf_override;
};

#define PGTABLE_WALK_DONE      1

struct pkvm_pgtable_walker {
	const pgtable_visit_fn_t cb;
	void *const arg;
	unsigned long flags;
#define PKVM_PGTABLE_WALK_TABLE_PRE	BIT(0)
#define PKVM_PGTABLE_WALK_LEAF		BIT(1)
#define PKVM_PGTABLE_WALK_TABLE_POST	BIT(2)
};

int pgtable_walk(struct pkvm_pgtable *pgt, unsigned long vaddr,
		unsigned long size, bool page_aligned,
		struct pkvm_pgtable_walker *walker);
int pkvm_pgtable_init(struct pkvm_pgtable *pgt,
		struct pkvm_mm_ops *mm_ops,
		struct pkvm_pgtable_ops *pgt_ops,
		struct pkvm_pgtable_cap *cap,
		bool alloc_root);
int pkvm_pgtable_map(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
		unsigned long phys_start, unsigned long size,
		int pgsz_mask, u64 entry_prot, pgtable_leaf_ov_fn_t map_leaf);
int pkvm_pgtable_unmap(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
		       unsigned long size, pgtable_leaf_ov_fn_t unmap_leaf);
int pkvm_pgtable_unmap_safe(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
			    unsigned long phys_start, unsigned long size,
			    pgtable_leaf_ov_fn_t unmap_leaf);
int pkvm_pgtable_unmap_nosplit(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
		       unsigned long size, pgtable_leaf_ov_fn_t unmap_leaf);
void pkvm_pgtable_lookup(struct pkvm_pgtable *pgt, unsigned long vaddr,
		unsigned long *pphys, u64 *pprot, int *plevel);
void pkvm_pgtable_destroy(struct pkvm_pgtable *pgt, pgtable_leaf_ov_fn_t free_leaf);
int pkvm_pgtable_annotate(struct pkvm_pgtable *pgt, unsigned long addr,
			  unsigned long size, u64 annotation);
int pkvm_pgtable_sync_map(struct pkvm_pgtable *src, struct pkvm_pgtable *dest,
			  u64 *prot, pgtable_leaf_ov_fn_t map_leaf);

static inline void pkvm_pgtable_set_mm_ops(struct pkvm_pgtable *pgt, struct pkvm_mm_ops *mm_ops)
{
	pgt->mm_ops = mm_ops;
}
#endif
