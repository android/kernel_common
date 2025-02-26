// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU-agnostic ARM page table allocator.
 * Host-specific functions. The rest is in io-pgtable-arm-common.c.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt)	"arm-lpae io-pgtable: " fmt

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/io-pgtable-arm.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>

#include "iommu-pages.h"

#include <asm/barrier.h>

static bool selftest_running = false;

int arm_lpae_map_exists(void)
{
	WARN_ON(!selftest_running);
	return -EEXIST;
}

void arm_lpae_unmap_empty(void)
{
	WARN_ON(!selftest_running);
}

static dma_addr_t __arm_lpae_dma_addr(void *pages)
{
	return (dma_addr_t)virt_to_phys(pages);
}

void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
			     struct io_pgtable_cfg *cfg,
			     void *cookie)
{
	struct device *dev = cfg->iommu_dev;
	int order = get_order(size);
	dma_addr_t dma;
	void *pages;

	VM_BUG_ON((gfp & __GFP_HIGHMEM));

	if (cfg->alloc)
		pages = cfg->alloc(cookie, size, gfp);
	else
		pages = iommu_alloc_pages_node(dev_to_node(dev), gfp, order);

	if (!pages)
		return NULL;

	if (!cfg->coherent_walk) {
		dma = dma_map_single(dev, pages, size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, dma))
			goto out_free;
		/*
		 * We depend on the IOMMU being able to work with any physical
		 * address directly, so if the DMA layer suggests otherwise by
		 * translating or truncating them, that bodes very badly...
		 */
		if (dma != virt_to_phys(pages))
			goto out_unmap;
	}

	return pages;

out_unmap:
	dev_err(dev, "Cannot accommodate DMA translation for IOMMU page tables\n");
	dma_unmap_single(dev, dma, size, DMA_TO_DEVICE);

out_free:
	if (cfg->free)
		cfg->free(cookie, pages, size);
	else
		iommu_free_pages(pages, order);

	return NULL;
}

void __arm_lpae_free_pages(void *pages, size_t size,
			   struct io_pgtable_cfg *cfg,
			   void *cookie)
{
	if (!cfg->coherent_walk)
		dma_unmap_single(cfg->iommu_dev, __arm_lpae_dma_addr(pages),
				 size, DMA_TO_DEVICE);

	if (cfg->free)
		cfg->free(cookie, pages, size);
	else
		iommu_free_pages(pages, get_order(size));
}

void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
			 struct io_pgtable_cfg *cfg)
{
	dma_sync_single_for_device(cfg->iommu_dev, __arm_lpae_dma_addr(ptep),
				   sizeof(*ptep) * num_entries, DMA_TO_DEVICE);
}

static void arm_lpae_free_pgtable(struct io_pgtable *iop)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_to_data(iop);

	__arm_lpae_free_pgtable(data, data->start_level, data->pgd);
	kfree(data);
}

static int visit_dirty(struct io_pgtable_walk_data *walk_data, int lvl,
		       arm_lpae_iopte *ptep, size_t size)
{
	struct io_pgtable_walk_common *walker = walk_data->data;
	struct iommu_dirty_bitmap *dirty = walker->data;

	if (!iopte_leaf(*ptep, lvl, walk_data->iop->fmt))
		return 0;

	if (iopte_writeable_dirty(*ptep)) {
		iommu_dirty_bitmap_record(dirty, walk_data->addr, size);
		if (!(walk_data->flags & IOMMU_DIRTY_NO_CLEAR))
			iopte_set_writeable_clean(ptep);
	}

	return 0;
}

static int arm_lpae_read_and_clear_dirty(struct io_pgtable_ops *ops,
					 unsigned long iova, size_t size,
					 unsigned long flags,
					 struct iommu_dirty_bitmap *dirty)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	struct io_pgtable_walk_common walker = {
		.data = dirty,
	};
	struct io_pgtable_walk_data walk_data = {
		.iop = &data->iop,
		.data = &walker,
		.visit = visit_dirty,
		.flags = flags,
		.addr = iova,
		.end = iova + size,
	};
	arm_lpae_iopte *ptep = data->pgd;
	int lvl = data->start_level;

	if (WARN_ON(!size))
		return -EINVAL;
	if (WARN_ON((iova + size - 1) & ~(BIT(cfg->ias) - 1)))
		return -EINVAL;
	if (data->iop.fmt != ARM_64_LPAE_S1)
		return -EINVAL;

	return __arm_lpae_iopte_walk(data, &walk_data, ptep, lvl);
}

static struct io_pgtable *
arm_64_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct arm_lpae_io_pgtable *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	if (arm_lpae_init_pgtable_s1(cfg, data))
		goto out_free_data;

	data->iop.ops.read_and_clear_dirty = arm_lpae_read_and_clear_dirty;
	/* Looking good; allocate a pgd */
	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data),
					   GFP_KERNEL, cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	/* TTBR */
	cfg->arm_lpae_s1_cfg.ttbr = virt_to_phys(data->pgd);
	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

static int arm_64_lpae_configure_s1(struct io_pgtable_cfg *cfg)
{
	struct arm_lpae_io_pgtable data = {};

	return arm_lpae_init_pgtable_s1(cfg, &data);
}

static struct io_pgtable *
arm_64_lpae_alloc_pgtable_s2(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct arm_lpae_io_pgtable *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	if (arm_lpae_init_pgtable_s2(cfg, data))
		goto out_free_data;

	data->iop.ops.read_and_clear_dirty = arm_lpae_read_and_clear_dirty;
	/* Allocate pgd pages */
	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data),
					   GFP_KERNEL, cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	/* VTTBR */
	cfg->arm_lpae_s2_cfg.vttbr = virt_to_phys(data->pgd);
	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

static int arm_64_lpae_configure_s2(struct io_pgtable_cfg *cfg)
{
	struct arm_lpae_io_pgtable data = {};

	return arm_lpae_init_pgtable_s2(cfg, &data);
}

static struct io_pgtable *
arm_32_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
	if (cfg->ias > 32 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);
	return arm_64_lpae_alloc_pgtable_s1(cfg, cookie);
}

static struct io_pgtable *
arm_32_lpae_alloc_pgtable_s2(struct io_pgtable_cfg *cfg, void *cookie)
{
	if (cfg->ias > 40 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);
	return arm_64_lpae_alloc_pgtable_s2(cfg, cookie);
}

static struct io_pgtable *
arm_mali_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct arm_lpae_io_pgtable *data;

	/* No quirks for Mali (hopefully) */
	if (cfg->quirks)
		return NULL;

	if (cfg->ias > 48 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	if (arm_lpae_init_pgtable(cfg, data))
		return NULL;

	data->iop.ops.read_and_clear_dirty = arm_lpae_read_and_clear_dirty;
	/* Mali seems to need a full 4-level table regardless of IAS */
	if (data->start_level > 0) {
		data->start_level = 0;
		data->pgd_bits = 0;
	}
	/*
	 * MEMATTR: Mali has no actual notion of a non-cacheable type, so the
	 * best we can do is mimic the out-of-tree driver and hope that the
	 * "implementation-defined caching policy" is good enough. Similarly,
	 * we'll use it for the sake of a valid attribute for our 'device'
	 * index, although callers should never request that in practice.
	 */
	cfg->arm_mali_lpae_cfg.memattr =
		(ARM_MALI_LPAE_MEMATTR_IMP_DEF
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_NC)) |
		(ARM_MALI_LPAE_MEMATTR_WRITE_ALLOC
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_CACHE)) |
		(ARM_MALI_LPAE_MEMATTR_IMP_DEF
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_DEV));

	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data), GFP_KERNEL,
					   cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before TRANSTAB can be written */
	wmb();

	cfg->arm_mali_lpae_cfg.transtab = virt_to_phys(data->pgd) |
					  ARM_MALI_LPAE_TTBR_READ_INNER |
					  ARM_MALI_LPAE_TTBR_ADRMODE_TABLE;
	if (cfg->coherent_walk)
		cfg->arm_mali_lpae_cfg.transtab |= ARM_MALI_LPAE_TTBR_SHARE_OUTER;

	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s1_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_64_lpae_alloc_pgtable_s1,
	.free	= arm_lpae_free_pgtable,
	.configure	= arm_64_lpae_configure_s1,
};

struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s2_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_64_lpae_alloc_pgtable_s2,
	.free	= arm_lpae_free_pgtable,
	.configure	= arm_64_lpae_configure_s2,
};

struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s1_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_32_lpae_alloc_pgtable_s1,
	.free	= arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s2_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_32_lpae_alloc_pgtable_s2,
	.free	= arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_mali_lpae_init_fns = {
	.caps	= IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc	= arm_mali_lpae_alloc_pgtable,
	.free	= arm_lpae_free_pgtable,
};

#ifdef CONFIG_IOMMU_IO_PGTABLE_LPAE_SELFTEST

static struct io_pgtable_cfg *cfg_cookie __initdata;
static struct io_pgtable_ops *cur_ops;

static void __init dummy_tlb_flush_all(void *cookie)
{
	WARN_ON(cookie != cfg_cookie);
}

static unsigned long skip_addr = 0xFFFF;
static void arm_lpae_selftest_validate(phys_addr_t addr, size_t size,
				       struct io_pgtable_walk_common *data,
				       void *wd)
{
	struct arm_lpae_io_pgtable_walk_data *arm_wd = data->data;
	unsigned long *iova = (unsigned long *)(arm_wd->cookie);
	arm_lpae_iopte *ptep = wd;

	/* PASS */
	if (*iova == addr)
		*iova = *iova + size;

	WARN_ON(skip_addr == addr);
	WARN_ON(!(*ptep));
	*ptep = 0;
}

static void __init dummy_tlb_flush(unsigned long iova, size_t size,
				   size_t granule, void *cookie)

{
	unsigned long iova_cookie = iova;
	struct arm_lpae_io_pgtable_walk_data wd = {
		.cookie = &iova_cookie,
	};
	struct io_pgtable_walk_common walk_data = {
		.visit_leaf = arm_lpae_selftest_validate,
		.data = &wd,
	};

	if (cur_ops && (cfg_cookie->quirks & IO_PGTABLE_QUIRK_UNMAP_INVAL)) {
		/* Not straight forward to propagate failures, so WARN_ON is noisy enough. */
		cur_ops->pgtable_walk(cur_ops, iova, size, &walk_data);
	}

	WARN_ON(cookie != cfg_cookie);
	WARN_ON(!(size & cfg_cookie->pgsize_bitmap));
}

static void __init dummy_tlb_add_page(struct iommu_iotlb_gather *gather,
				      unsigned long iova, size_t granule,
				      void *cookie)
{
	WARN_ON(cookie != cfg_cookie);
	WARN_ON(!(granule & cfg_cookie->pgsize_bitmap));
}

static const struct iommu_flush_ops dummy_tlb_ops __initconst = {
	.tlb_flush_all	= dummy_tlb_flush_all,
	.tlb_flush_walk	= dummy_tlb_flush,
	.tlb_add_page	= dummy_tlb_add_page,
};

static void __init arm_lpae_dump_ops(struct io_pgtable_ops *ops)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;

	pr_err("cfg: pgsize_bitmap 0x%lx, ias %u-bit\n",
		cfg->pgsize_bitmap, cfg->ias);
	pr_err("data: %d levels, 0x%zx pgd_size, %u pg_shift, %u bits_per_level, pgd @ %p\n",
		ARM_LPAE_MAX_LEVELS - data->start_level, ARM_LPAE_PGD_SIZE(data),
		ilog2(ARM_LPAE_GRANULE(data)), data->bits_per_level, data->pgd);
}

#define __FAIL(ops, i)	({						\
		WARN(1, "selftest: test failed for fmt idx %d\n", (i));	\
		arm_lpae_dump_ops(ops);					\
		selftest_running = false;				\
		-EFAULT;						\
})

static int __init arm_lpae_run_tests(struct io_pgtable_cfg *cfg)
{
	static const enum io_pgtable_fmt fmts[] __initconst = {
		ARM_64_LPAE_S1,
		ARM_64_LPAE_S2,
	};

	int i, j;
	unsigned long iova, iova_cookie;
	size_t size, mapped;
	struct io_pgtable_ops *ops;
	struct arm_lpae_io_pgtable_walk_data arm_wd;
	struct io_pgtable_walk_common common_wd;
	int ret;

	selftest_running = true;

	for (i = 0; i < ARRAY_SIZE(fmts); ++i) {
		cfg_cookie = cfg;
		cfg->quirks = 0;
		ops = alloc_io_pgtable_ops(fmts[i], cfg, cfg);
		if (!ops) {
			pr_err("selftest: failed to allocate io pgtable ops\n");
			return -ENOMEM;
		}

		/*
		 * Initial sanity checks.
		 * Empty page tables shouldn't provide any translations.
		 */
		if (ops->iova_to_phys(ops, 42))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_1G + 42))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_2G + 42))
			return __FAIL(ops, i);

		/*
		 * Distinct mappings of different granule sizes.
		 */
		iova = 0;
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {
			size = 1UL << j;

			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_READ | IOMMU_WRITE |
					   IOMMU_NOEXEC | IOMMU_CACHE,
					   GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			/* Overlapping mappings */
			if (!ops->map_pages(ops, iova, iova + size, size, 1,
					    IOMMU_READ | IOMMU_NOEXEC,
					    GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
				return __FAIL(ops, i);

			iova += SZ_1G;
		}

		/* Partial unmap */
		size = 1UL << __ffs(cfg->pgsize_bitmap);
		if (ops->unmap_pages(ops, SZ_1G + size, size, 1, NULL) != size)
			return __FAIL(ops, i);

		/* Remap of partial unmap */
		if (ops->map_pages(ops, SZ_1G + size, size, size, 1,
				   IOMMU_READ, GFP_KERNEL, &mapped))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_1G + size + 42) != (size + 42))
			return __FAIL(ops, i);

		/* Full unmap */
		iova = 0;
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {
			size = 1UL << j;

			if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42))
				return __FAIL(ops, i);

			/* Remap full block */
			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_WRITE, GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
				return __FAIL(ops, i);

			iova += SZ_1G;
		}

		free_io_pgtable_ops(ops);

		/* Test: IO_PGTABLE_QUIRK_UNMAP_INVAL */
		cfg->quirks = IO_PGTABLE_QUIRK_UNMAP_INVAL;
		ops = alloc_io_pgtable_ops(fmts[i], cfg, cfg);
		cur_ops = ops;
		if (!ops) {
			pr_err("selftest: failed to allocate io pgtable ops with IO_PGTABLE_QUIRK_UNMAP_INVAL\n");
			return -ENOMEM;
		}

		common_wd.visit_leaf = arm_lpae_selftest_validate;
		common_wd.data = &arm_wd;
		arm_wd.cookie = &iova_cookie;

		/*
		 * Map with leaf size => unmap with leaf size
		 * Then walk the table to check the pages
		 */
		size = 1UL << __ffs(cfg->pgsize_bitmap);
		iova = size * 3; /* Arbitrary aligned address. */
		if (ops->map_pages(ops, iova, iova, size, 1,
				   IOMMU_READ | IOMMU_WRITE |
				   IOMMU_NOEXEC | IOMMU_CACHE,
				   GFP_KERNEL, &mapped))
			return __FAIL(ops, i);

		if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
			return __FAIL(ops, i);

		iova_cookie = iova;
		ret = ops->pgtable_walk(ops, iova, size, &common_wd);
		if (ret || (iova_cookie != iova + size))
			return __FAIL(ops, i);

		/*
		 * Map with leaf size => partial unmap with leaf size
		 * Then walk the table to check the pages
		 */
		if (ops->map_pages(ops, iova, iova, size, 42,
				   IOMMU_READ | IOMMU_WRITE |
				   IOMMU_NOEXEC | IOMMU_CACHE,
				   GFP_KERNEL, &mapped))
			return __FAIL(ops, i);

		if (ops->unmap_pages(ops, iova + 41 * size, size, 1, NULL) != size)
			return __FAIL(ops, i);

		iova_cookie = iova + 41 * size;
		ret = ops->pgtable_walk(ops, iova_cookie, size, &common_wd);
		if (ret || (iova_cookie != (iova + 42 * size)))
			return __FAIL(ops, i);

		if (ops->unmap_pages(ops, iova, size, 41, NULL) != 41 * size)
			return __FAIL(ops, i);

		iova_cookie = iova;
		ret = ops->pgtable_walk(ops, iova, size * 41, &common_wd);
		if (ret || (iova_cookie != (iova + 41 * size)))
			return __FAIL(ops, i);
		/*
		 * Distinct mappings of different granule sizes.
		 */
		iova = 0;
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {
			size = 1UL << j;

			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_READ | IOMMU_WRITE |
					   IOMMU_NOEXEC | IOMMU_CACHE,
					   GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			/* Overlapping mappings */
			if (!ops->map_pages(ops, iova, iova + size, size, 1,
					    IOMMU_READ | IOMMU_NOEXEC,
					    GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
				return __FAIL(ops, i);

			iova += SZ_1G;
		}

		/* Partial unmap (split blk) */
		size = 1UL << __ffs(cfg->pgsize_bitmap);
		iova = SZ_1G + size;
		if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
			return __FAIL(ops, i);

		/* Now we have table instead of block, with missing entry let's see */
		iova_cookie = iova;
		ret = ops->pgtable_walk(ops, iova, size, &common_wd);
		if (ret || (iova_cookie != (iova + size)))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, iova))
			return __FAIL(ops, i);

		/*
		 * Let's replace with a block again.
		 * We expect the freed table will be called in tlb_flush_walk()
		 * that's how we can track the unmapped pages.
		 */
		size = 1ULL << __ffs(cfg->pgsize_bitmap & ~(1UL << __ffs(cfg->pgsize_bitmap)));
		/* Already unmapped shouldn't walk it again! */
		skip_addr = SZ_1G + size;
		iova = SZ_1G;
		iova_cookie = iova;
		if (ops->map_pages(ops, iova, iova, size, 1,
				   IOMMU_READ, GFP_KERNEL, &mapped))
			return __FAIL(ops, i);
		skip_addr = 0XFFFF;

		/* Let's break the block to table again, this time at the start. */
		size = 1UL << __ffs(cfg->pgsize_bitmap);

		if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
			return __FAIL(ops, i);

		/* Now we have table instead of block, with missing entry let's see */
		iova_cookie = iova;
		ret = ops->pgtable_walk(ops, iova_cookie, size, &common_wd);
		if (ret || (iova_cookie != (iova + size)))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, iova))
			return __FAIL(ops, i);

		/* Let's unmap the whole table at once. */
		size = 1ULL << __ffs(cfg->pgsize_bitmap & ~(1UL << __ffs(cfg->pgsize_bitmap)));
		skip_addr = iova;
		if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
			return __FAIL(ops, i);
		skip_addr = 0xFFFF;
		cur_ops = NULL;
		free_io_pgtable_ops(ops);
	}

	selftest_running = false;
	return 0;
}

static int __init arm_lpae_do_selftests(void)
{
	static const unsigned long pgsize[] __initconst = {
		SZ_4K | SZ_2M | SZ_1G,
		SZ_16K | SZ_32M,
		SZ_64K | SZ_512M,
	};

	static const unsigned int ias[] __initconst = {
		32, 36, 40, 42, 44, 48,
	};

	int i, j, pass = 0, fail = 0;
	struct device dev;
	struct io_pgtable_cfg cfg = {
		.tlb = &dummy_tlb_ops,
		.oas = 48,
		.coherent_walk = true,
		.iommu_dev = &dev,
	};

	/* __arm_lpae_alloc_pages() merely needs dev_to_node() to work */
	set_dev_node(&dev, NUMA_NO_NODE);

	for (i = 0; i < ARRAY_SIZE(pgsize); ++i) {
		for (j = 0; j < ARRAY_SIZE(ias); ++j) {
			cfg.pgsize_bitmap = pgsize[i];
			cfg.ias = ias[j];
			pr_info("selftest: pgsize_bitmap 0x%08lx, IAS %u\n",
				pgsize[i], ias[j]);
			if (arm_lpae_run_tests(&cfg))
				fail++;
			else
				pass++;
		}
	}

	pr_info("selftest: completed with %d PASS %d FAIL\n", pass, fail);
	return fail ? -EFAULT : 0;
}
subsys_initcall(arm_lpae_do_selftests);
#endif
