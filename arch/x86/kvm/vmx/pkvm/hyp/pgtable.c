// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <pkvm.h>

#include "pgtable.h"
#include "memory.h"
#include "debug.h"
#include "bug.h"

struct pgt_walk_data {
	struct pkvm_pgtable *pgt;
	struct pgt_flush_data flush_data;
	unsigned long vaddr;
	unsigned long vaddr_end;
	struct pkvm_pgtable_walker *walker;
};

struct pkvm_pgtable_map_data {
	unsigned long phys;
	u64 annotation;
	u64 prot;
	int pgsz_mask;
};

struct pkvm_pgtable_unmap_data {
	unsigned long phys;
};

struct pkvm_pgtable_lookup_data {
	unsigned long vaddr;
	unsigned long phys;
	u64 prot;
	int level;
};

static bool pkvm_phys_is_valid(u64 phys)
{
	return phys != INVALID_ADDR;
}

static bool pgtable_pte_is_counted(u64 pte)
{
	/*
	 * Due to we use the invalid pte to record the page ownership,
	 * the refcount tracks both valid and invalid pte if the pte is
	 * not 0.
	 */
	return !!pte;
}

static bool leaf_mapping_valid(struct pkvm_pgtable_ops *pgt_ops,
			       unsigned long vaddr,
			       unsigned long vaddr_end,
			       int pgsz_mask,
			       int level)
{
	unsigned long page_size = pgt_ops->pgt_level_to_size(level);

	if (!((1 << level) & pgsz_mask))
		return false;

	if (!IS_ALIGNED(vaddr, page_size))
		return false;

	if (page_size > (vaddr_end - vaddr))
		return false;

	return true;
}

static bool leaf_mapping_allowed(struct pkvm_pgtable_ops *pgt_ops,
				 unsigned long vaddr,
				 unsigned long vaddr_end,
				 unsigned long phys,
				 int pgsz_mask,
				 int level)
{
	unsigned long page_size = pgt_ops->pgt_level_to_size(level);

	if (pkvm_phys_is_valid(phys) && !IS_ALIGNED(phys, page_size))
		return false;

	return leaf_mapping_valid(pgt_ops, vaddr, vaddr_end, pgsz_mask, level);
}

static void pgtable_split(struct pkvm_pgtable_ops *pgt_ops,
			  struct pkvm_mm_ops *mm_ops,
			  unsigned long vaddr, unsigned long phys,
			  unsigned long size, void *ptep,
			  int level, u64 prot)
{
	unsigned long phys_end = phys + size;
	int level_size = pgt_ops->pgt_level_to_size(level);
	int entry_size = PAGE_SIZE / pgt_ops->pgt_level_to_entries(level);
	int i = 0;

	if (level > PG_LEVEL_4K)
		pgt_ops->pgt_entry_mkhuge(&prot);

	for (i = 0; phys < phys_end; phys += level_size, i++) {
		pgt_ops->pgt_set_entry((ptep + i * entry_size), phys | prot);
		mm_ops->get_page(ptep);
	}
}

static int pgtable_map_try_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr,
				unsigned long vaddr_end, int level, void *ptep,
				struct pgt_flush_data *flush_data,
				struct pkvm_pgtable_map_data *data)
{
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	struct pkvm_mm_ops *mm_ops = pgt->mm_ops;
	u64 old = *(u64 *)ptep, new;

	if (!leaf_mapping_allowed(pgt_ops, vaddr, vaddr_end,
				 data->phys, data->pgsz_mask, level)) {
		/* The 4K page shall be able to map, otherwise return err */
		return (level == PG_LEVEL_4K ? -EINVAL : -E2BIG);
	}

	if (pkvm_phys_is_valid(data->phys)) {
		new = data->phys | data->prot;
		if (level != PG_LEVEL_4K)
			pgt_ops->pgt_entry_mkhuge(&new);
	} else {
		new = data->annotation;
	}

	if (pgtable_pte_is_counted(old)) {
		if (pgt_ops->pgt_entry_present(ptep)) {
			pgt_ops->pgt_set_entry(ptep, 0);
			flush_data->flushtlb |= true;
		}
		mm_ops->put_page(ptep);
	}

	if (pgtable_pte_is_counted(new))
		mm_ops->get_page(ptep);

	pgt_ops->pgt_set_entry(ptep, new);
	if (pkvm_phys_is_valid(data->phys))
		data->phys += page_level_size(level);

	return 0;
}

static int pgtable_map_walk_leaf(struct pkvm_pgtable *pgt,
				 unsigned long vaddr, unsigned long vaddr_end,
				 int level, void *ptep, unsigned long flags,
				 struct pgt_flush_data *flush_data,
				 struct pkvm_pgtable_map_data *data)
{
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	struct pkvm_mm_ops *mm_ops = pgt->mm_ops;
	unsigned long size = page_level_size(level);
	void *page;
	int ret;

	/* First try to create leaf page mapping on current level */
	ret = pgtable_map_try_leaf(pgt, vaddr, vaddr_end, level, ptep, flush_data, data);
	if (ret != -E2BIG)
		return ret;

	/*
	 * Be here is because the mapping needs be done on smaller(or level-1)
	 * page size. We need to allocate a table page for the smaller(level-1)
	 * page mapping. And for current level, if the huge page mapping already
	 * present, we need further split it.
	 */
	page = mm_ops->zalloc_page();
	if (!page)
		return -ENOMEM;

	if (pgt_ops->pgt_entry_huge(ptep)) {
		/*
		 * Split the large mapping and reuse the
		 * large mapping's prot. The translation
		 * doesn't have a change, so no need to
		 * flush tlb.
		 */
		mm_ops->put_page(ptep);
		pgtable_split(pgt_ops, mm_ops, ALIGN_DOWN(vaddr, size),
			      pgt_ops->pgt_entry_to_phys(ptep),
			      size, page, level - 1,
			      pgt_ops->pgt_entry_to_prot(ptep));
	}

	mm_ops->get_page(ptep);
	pgt_ops->pgt_set_entry(ptep, pgt->table_prot | mm_ops->virt_to_phys(page));

	return 0;
}

/*
 *TODO: support merging small entries to a large one.
 */
static int pgtable_map_cb(struct pkvm_pgtable *pgt, unsigned long vaddr,
			  unsigned long vaddr_end, int level, void *ptep,
			  unsigned long flags, struct pgt_flush_data *flush_data,
			  void *const arg)
{
	struct pkvm_pgtable_map_data *data = arg;

	switch (flags) {
	case PKVM_PGTABLE_WALK_LEAF:
		return pgtable_map_walk_leaf(pgt, vaddr, vaddr_end, level,
					     ptep, flags, flush_data, data);
	case PKVM_PGTABLE_WALK_TABLE_PRE:
	case PKVM_PGTABLE_WALK_TABLE_POST:
		break;
	}

	return -EINVAL;
}

/*
 * put_page_to_free_list(): the page added to the freelist should not be used
 * by any one as this page will be used as a node linked to the freelist.
 */
static inline void put_page_to_freelist(void *page, struct list_head *head)
{
	struct list_head *node = page;

	list_add_tail(node, head);
}

/*
 * get_page_to_free_list(): the page got from the freelist is valid to be used
 * again.
 */
static inline void *get_page_from_freelist(struct list_head *head)
{
	struct list_head *node = head->next;

	list_del(node);
	memset(node, 0, sizeof(struct list_head));

	return (void *)node;
}

static int pgtable_unmap_cb(struct pkvm_pgtable *pgt, unsigned long vaddr,
			    unsigned long vaddr_end, int level, void *ptep,
			    unsigned long flags, struct pgt_flush_data *flush_data,
			    void *const arg)
{
	struct pkvm_pgtable_unmap_data *data = arg;
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	struct pkvm_mm_ops *mm_ops = pgt->mm_ops;
	unsigned long size = page_level_size(level);
	void *child_ptep;

	if (!pgt_ops->pgt_entry_present(ptep))
		/* Nothing to do if the entry is not present */
		return 0;

	/*
	 * Can direct unmap if matches with a large entry or a 4K entry
	 */
	if (level == PG_LEVEL_4K || (pgt_ops->pgt_entry_huge(ptep) &&
				     leaf_mapping_valid(pgt_ops, vaddr, vaddr_end,
							1 << level, level))) {
		if (data->phys != INVALID_ADDR) {
			unsigned long phys = pgt_ops->pgt_entry_to_phys(ptep);

			PKVM_ASSERT(phys == data->phys);
		}

		pgt_ops->pgt_set_entry(ptep, 0);
		flush_data->flushtlb |= true;
		mm_ops->put_page(ptep);

		if (data->phys != INVALID_ADDR) {
			data->phys = ALIGN_DOWN(data->phys, size);
			data->phys += size;
		}
		return 0;
	}

	if (pgt_ops->pgt_entry_huge(ptep)) {
		/*
		 * if it is huge pte, split and goto next level.
		 */
		void *page = mm_ops->zalloc_page();

		if (!page)
			return -ENOMEM;
		/*
		 * Split the large mapping and reuse the
		 * large mapping's prot. The translation
		 * doesn't have a change, so no need to
		 * flush tlb.
		 */
		pgtable_split(pgt_ops, mm_ops, ALIGN_DOWN(vaddr, size),
			      pgt_ops->pgt_entry_to_phys(ptep),
			      size, page, level - 1,
			      pgt_ops->pgt_entry_to_prot(ptep));
		pgt_ops->pgt_set_entry(ptep, pgt->table_prot | mm_ops->virt_to_phys(page));
		return 0;
	}

	/*
	 * if not huge entry then means it is table entry, then check
	 * the child pte page refcount. Put the child pte page if no
	 * one else is using it.
	 */
	child_ptep = mm_ops->phys_to_virt(pgt_ops->pgt_entry_to_phys(ptep));
	if (mm_ops->page_count(child_ptep) == 1) {
		pgt_ops->pgt_set_entry(ptep, 0);
		mm_ops->put_page(ptep);
		put_page_to_freelist(child_ptep, &flush_data->free_list);
	}

	return 0;
}

static int pgtable_lookup_cb(struct pkvm_pgtable *pgt,
			    unsigned long aligned_vaddr,
			    unsigned long aligned_vaddr_end,
			    int level,
			    void *ptep,
			    unsigned long flags,
			    struct pgt_flush_data *flush_data,
			    void *const arg)
{
	struct pkvm_pgtable_lookup_data *data = arg;
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	u64 pte = atomic64_read((atomic64_t *)ptep);

	data->phys = INVALID_ADDR;
	data->prot = 0;
	data->level = level;

	/*
	 * This cb shall only be called for leaf, if now it is not a leaf
	 * that means the pte is changed by others, we shall re-walk the pgtable
	 */
	if (unlikely(!pgt_ops->pgt_entry_is_leaf(&pte, level)))
		return -EAGAIN;

	if (pgt_ops->pgt_entry_present(&pte)) {
		unsigned long offset =
			data->vaddr & ~pgt_ops->pgt_level_page_mask(level);

		data->phys = pgt_ops->pgt_entry_to_phys(&pte) + offset;
		data->prot = pgt_ops->pgt_entry_to_prot(&pte);
	}

	return PGTABLE_WALK_DONE;
}

static int pgtable_free_cb(struct pkvm_pgtable *pgt,
			    unsigned long vaddr,
			    unsigned long vaddr_end,
			    int level,
			    void *ptep,
			    unsigned long flags,
			    struct pgt_flush_data *flush_data,
			    void *const arg)
{
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	struct pkvm_mm_ops *mm_ops = pgt->mm_ops;
	phys_addr_t phys;
	void *virt;

	if (pgt_ops->pgt_entry_is_leaf(ptep, level)) {
		if (pgt_ops->pgt_entry_present(ptep)) {
			flush_data->flushtlb |= true;
			mm_ops->put_page(ptep);
		}
		return 0;
	}

	/* Free the child page */
	phys = pgt_ops->pgt_entry_to_phys(ptep);
	virt = mm_ops->phys_to_virt(phys);
	if (mm_ops->page_count(virt) == 1) {
		pgt_ops->pgt_set_entry(ptep, 0);
		mm_ops->put_page(ptep);
		put_page_to_freelist(virt, &flush_data->free_list);
	}

	return 0;
}

static int _pgtable_walk(struct pgt_walk_data *data, void *ptep, int level);
static int pgtable_visit(struct pgt_walk_data *data, void *ptep, int level)
{
	struct pkvm_pgtable_ops *pgt_ops = data->pgt->pgt_ops;
	struct pkvm_mm_ops *mm_ops = data->pgt->mm_ops;
	struct pkvm_pgtable_walker *walker = data->walker;
	unsigned long flags = walker->flags;
	bool leaf = pgt_ops->pgt_entry_is_leaf(ptep, level);
	void *child_ptep;
	int ret = 0;

	if (!leaf && (flags & PKVM_PGTABLE_WALK_TABLE_PRE))
		ret = walker->cb(data->pgt, data->vaddr, data->vaddr_end,
				 level, ptep, PKVM_PGTABLE_WALK_TABLE_PRE,
				 &data->flush_data, walker->arg);

	if (leaf && (flags & PKVM_PGTABLE_WALK_LEAF)) {
		ret = walker->cb(data->pgt, data->vaddr, data->vaddr_end,
				 level, ptep, PKVM_PGTABLE_WALK_LEAF,
				 &data->flush_data, walker->arg);
		leaf = pgt_ops->pgt_entry_is_leaf(ptep, level);
	}

	if (ret)
		return ret;

	if (leaf) {
		unsigned long size = pgt_ops->pgt_level_to_size(level);

		data->vaddr = ALIGN_DOWN(data->vaddr, size);
		data->vaddr += size;
		return ret;
	}

	child_ptep = mm_ops->phys_to_virt(pgt_ops->pgt_entry_to_phys(ptep));
	ret = _pgtable_walk(data, child_ptep, level - 1);
	if (ret)
		return ret;

	if (flags & PKVM_PGTABLE_WALK_TABLE_POST)
		ret = walker->cb(data->pgt, data->vaddr, data->vaddr_end,
				 level, ptep, PKVM_PGTABLE_WALK_TABLE_POST,
				 &data->flush_data, walker->arg);

	return ret;
}

static int _pgtable_walk(struct pgt_walk_data *data, void *ptep, int level)
{
	struct pkvm_pgtable_ops *pgt_ops = data->pgt->pgt_ops;
	int entries = pgt_ops->pgt_level_to_entries(level);
	int entry_size = pgt_ops->pgt_level_entry_size(level);
	int idx = pgt_ops->pgt_entry_to_index(data->vaddr, level);
	int ret;

	for (; idx < entries; idx++) {
		if (data->vaddr >= data->vaddr_end)
			break;

		ret = pgtable_visit(data, (ptep + idx * entry_size), level);
		if (ret)
			return ret;
	}

	return 0;
}

int pgtable_walk(struct pkvm_pgtable *pgt, unsigned long vaddr,
			unsigned long size, struct pkvm_pgtable_walker *walker)
{
	unsigned long aligned_vaddr = ALIGN_DOWN(vaddr, PAGE_SIZE);
	struct pgt_walk_data data = {
		.pgt = pgt,
		.flush_data = {
			.flushtlb = false,
			.free_list = LIST_HEAD_INIT(data.flush_data.free_list),
		},
		.vaddr = aligned_vaddr,
		.vaddr_end = aligned_vaddr + ALIGN(size, PAGE_SIZE),
		.walker = walker,
	};
	struct pkvm_mm_ops *mm_ops = pgt->mm_ops;
	int ret;

	if (!size || data.vaddr == data.vaddr_end)
		return 0;

	ret = _pgtable_walk(&data, mm_ops->phys_to_virt(pgt->root_pa), pgt->level);

	if (data.flush_data.flushtlb || !list_empty(&data.flush_data.free_list))
		pgt->mm_ops->flush_tlb();

	while (!list_empty(&data.flush_data.free_list)) {
		void *page = get_page_from_freelist(&data.flush_data.free_list);

		pgt->mm_ops->put_page(page);
	}

	return ret;
}

int pkvm_pgtable_init(struct pkvm_pgtable *pgt,
			     struct pkvm_mm_ops *mm_ops,
			     struct pkvm_pgtable_ops *pgt_ops,
			     struct pkvm_pgtable_cap *cap,
			     bool alloc_root)
{
	void *root;

	if (!mm_ops || !pgt_ops || !cap)
		return -EINVAL;

	if (alloc_root && mm_ops->zalloc_page) {
		root = mm_ops->zalloc_page();
		if (!root)
			return -ENOMEM;
		pgt->root_pa = __pkvm_pa(root);
	}

	pgt->mm_ops = mm_ops;
	pgt->pgt_ops = pgt_ops;
	pgt->level = cap->level;
	pgt->allowed_pgsz = cap->allowed_pgsz;
	pgt->table_prot = cap->table_prot;

	return 0;
}

static int __pkvm_pgtable_map(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
		     unsigned long phys, unsigned long size,
		     int pgsz_mask, u64 prot, u64 annotation)
{
	struct pkvm_pgtable_map_data data = {
		.phys = phys,
		.annotation = annotation,
		.prot = prot,
		.pgsz_mask = pgsz_mask ? pgt->allowed_pgsz & pgsz_mask :
					 pgt->allowed_pgsz,
	};
	struct pkvm_pgtable_walker walker = {
		.cb = pgtable_map_cb,
		.arg = &data,
		.flags = PKVM_PGTABLE_WALK_LEAF,
	};

	return pgtable_walk(pgt, vaddr_start, size, &walker);
}

int pkvm_pgtable_map(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
		     unsigned long phys_start, unsigned long size,
		     int pgsz_mask, u64 prot)
{
	return __pkvm_pgtable_map(pgt, vaddr_start, ALIGN_DOWN(phys_start, PAGE_SIZE),
				  size, pgsz_mask, prot, 0);
}

int pkvm_pgtable_unmap(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
		       unsigned long size)
{
	struct pkvm_pgtable_unmap_data data = {
		.phys = INVALID_ADDR,
	};
	struct pkvm_pgtable_walker walker = {
		.cb = pgtable_unmap_cb,
		.arg = &data,
		.flags = PKVM_PGTABLE_WALK_LEAF | PKVM_PGTABLE_WALK_TABLE_POST,
	};

	return pgtable_walk(pgt, vaddr_start, size, &walker);
}

int pkvm_pgtable_unmap_safe(struct pkvm_pgtable *pgt, unsigned long vaddr_start,
			    unsigned long phys_start, unsigned long size)
{
	struct pkvm_pgtable_unmap_data data = {
		.phys = ALIGN_DOWN(phys_start, PAGE_SIZE),
	};
	struct pkvm_pgtable_walker walker = {
		.cb = pgtable_unmap_cb,
		.arg = &data,
		.flags = PKVM_PGTABLE_WALK_LEAF | PKVM_PGTABLE_WALK_TABLE_POST,
	};

	return pgtable_walk(pgt, vaddr_start, size, &walker);
}

void pkvm_pgtable_lookup(struct pkvm_pgtable *pgt, unsigned long vaddr,
		     unsigned long *pphys, u64 *pprot, int *plevel)
{
	struct pkvm_pgtable_lookup_data data = {
		.vaddr = vaddr,
	};
	struct pkvm_pgtable_walker walker = {
		.cb = pgtable_lookup_cb,
		.arg = &data,
		.flags = PKVM_PGTABLE_WALK_LEAF,
	};
	int ret, retry_cnt = 0;

retry:
	ret = pgtable_walk(pgt, vaddr, PAGE_SIZE, &walker);
	/*
	 * we give 5 times chance to re-walk pgtable if others change the
	 * PTE during above pgtable walk.
	 */
	if ((ret == -EAGAIN) && (retry_cnt++ < 5))
		goto retry;

	if (pphys)
		*pphys = data.phys;
	if (pprot)
		*pprot = data.prot;
	if (plevel)
		*plevel = data.level;
}

void pkvm_pgtable_destroy(struct pkvm_pgtable *pgt)
{
	unsigned long size;
	void *virt_root;
	struct pkvm_pgtable_ops *pgt_ops;
	struct pkvm_pgtable_walker walker = {
		.cb 	= pgtable_free_cb,
		.flags 	= PKVM_PGTABLE_WALK_LEAF | PKVM_PGTABLE_WALK_TABLE_POST,
	};

	pgt_ops = pgt->pgt_ops;
	size = pgt_ops->pgt_level_to_size(pgt->level + 1);

	pgtable_walk(pgt, 0, size, &walker);
	virt_root = pgt->mm_ops->phys_to_virt(pgt->root_pa);
	pgt->mm_ops->put_page(virt_root);
}

/*
 * pkvm_pgtable_annotate() - Unmap and annotate pages to track ownership.
 * @annotation:		The value stored in the invalid pte.
 * 			@annotation[2:0] must be 0.
 */
int pkvm_pgtable_annotate(struct pkvm_pgtable *pgt, unsigned long addr,
			  unsigned long size, u64 annotation)
{
	if (pgt->pgt_ops->pgt_entry_present(&annotation))
		return -EINVAL;

	return __pkvm_pgtable_map(pgt, addr, INVALID_ADDR, size,
			1 << PG_LEVEL_4K, 0, annotation);
}
