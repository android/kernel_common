// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Arm Ltd.
 */
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <linux/types.h>
#include <linux/gfp_types.h>
#include <linux/io-pgtable-arm.h>

#include <nvhe/alloc.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>

#include "arm_smmu_v3.h"
#include "arm-smmu-v3-module.h"

#define io_pgtable_cfg_to_pgtable(x) container_of((x), struct io_pgtable, cfg)

#define io_pgtable_cfg_to_data(x)					\
	io_pgtable_to_data(io_pgtable_cfg_to_pgtable(x))

int arm_lpae_map_exists(void)
{
	return -EEXIST;
}

void arm_lpae_unmap_empty(void)
{
}

void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
			     struct io_pgtable_cfg *cfg, void *cookie)
{
	void *addr;
	struct arm_lpae_io_pgtable *data = io_pgtable_cfg_to_data(cfg);

	if (!PAGE_ALIGNED(size))
		return NULL;

	if (data->idmapped)
		addr = kvm_iommu_donate_pages_atomic(get_order(size));
	else
		addr = kvm_iommu_donate_pages(get_order(size), 0);

	if (addr && !cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	return addr;
}

void __arm_lpae_free_pages(void *addr, size_t size, struct io_pgtable_cfg *cfg,
			   void *cookie)
{
	u8 order;
	struct arm_lpae_io_pgtable *data = io_pgtable_cfg_to_data(cfg);

	/*
	 * It's guaranteed all allocations are aligned, but core code
	 * might free PGD with it's actual size.
	 */
	order = get_order(PAGE_ALIGN(size));

	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	if (data->idmapped)
		kvm_iommu_reclaim_pages_atomic(addr, order);
	else
		kvm_iommu_reclaim_pages(addr, order);
}

void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
			 struct io_pgtable_cfg *cfg)
{
	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(ptep, sizeof(*ptep) * num_entries);
}

static int kvm_arm_io_pgtable_init(struct io_pgtable_cfg *cfg,
				   struct arm_lpae_io_pgtable *data)
{
	int ret = -EINVAL;

	if (cfg->fmt == ARM_64_LPAE_S2)
		ret = arm_lpae_init_pgtable_s2(cfg, data);
	else if (cfg->fmt == ARM_64_LPAE_S1)
		ret = arm_lpae_init_pgtable_s1(cfg, data);

	if (ret)
		return ret;

	data->iop.cfg = *cfg;
	data->iop.fmt	= cfg->fmt;

	return 0;
}

struct io_pgtable *kvm_arm_io_pgtable_alloc(struct io_pgtable_cfg *cfg,
					    void *cookie,
					    int *out_ret)
{
	size_t pgd_size, alignment;
	struct arm_lpae_io_pgtable *data;
	int ret;

	data = hyp_alloc(sizeof(*data));
	if (!data) {
		*out_ret = hyp_alloc_errno();
		return NULL;
	}

	ret = kvm_arm_io_pgtable_init(cfg, data);
	if (ret)
		goto out_free;

	pgd_size = PAGE_ALIGN(ARM_LPAE_PGD_SIZE(data));
	data->pgd = __arm_lpae_alloc_pages(pgd_size, 0, &data->iop.cfg, cookie);
	if (!data->pgd) {
		ret = -ENOMEM;
		goto out_free;
	}
	/*
	 * If it has eight or more entries, the table must be aligned on
	 * its size. Otherwise 64 bytes.
	 */
	alignment = max(pgd_size, 8 * sizeof(arm_lpae_iopte));
	if (!IS_ALIGNED(hyp_virt_to_phys(data->pgd), alignment)) {
		__arm_lpae_free_pages(data->pgd, pgd_size,
				      &data->iop.cfg, cookie);
		ret = -EINVAL;
		goto out_free;
	}

	data->iop.cookie = cookie;
	if (cfg->fmt == ARM_64_LPAE_S2)
		data->iop.cfg.arm_lpae_s2_cfg.vttbr = __arm_lpae_virt_to_phys(data->pgd);
	else if (cfg->fmt == ARM_64_LPAE_S1)
		data->iop.cfg.arm_lpae_s1_cfg.ttbr = __arm_lpae_virt_to_phys(data->pgd);

	if (!data->iop.cfg.coherent_walk)
		kvm_flush_dcache_to_poc(data->pgd, pgd_size);

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	*out_ret = 0;
	return &data->iop;
out_free:
	hyp_free(data);
	*out_ret = ret;
	return NULL;
}

int kvm_arm_io_pgtable_free(struct io_pgtable *iopt)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_to_data(iopt);
	size_t pgd_size = ARM_LPAE_PGD_SIZE(data);

	if (!data->iop.cfg.coherent_walk)
		kvm_flush_dcache_to_poc(data->pgd, pgd_size);

	io_pgtable_tlb_flush_all(iopt);
	__arm_lpae_free_pgtable(data, data->start_level, data->pgd);
	hyp_free(data);
	return 0;
}
