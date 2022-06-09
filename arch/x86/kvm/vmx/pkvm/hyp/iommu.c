/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#include <../drivers/iommu/intel/iommu.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>
#include "pkvm_hyp.h"
#include "gfp.h"
#include "memory.h"
#include "mmu.h"
#include "ept.h"
#include "pgtable.h"
#include "iommu_internal.h"
#include "debug.h"

#define for_each_valid_iommu(p)					\
	for (p = iommus; p < iommus + PKVM_MAX_IOMMU_NUM; p++)	\
		if (!p || !p->iommu.reg_phys) {			\
			continue;				\
		} else

static struct pkvm_iommu iommus[PKVM_MAX_IOMMU_NUM];

static struct hyp_pool iommu_pool;

static void *iommu_zalloc_page(void)
{
	return hyp_alloc_pages(&iommu_pool, 0);
}

static void iommu_get_page(void *vaddr)
{
	hyp_get_page(&iommu_pool, vaddr);
}

static void iommu_put_page(void *vaddr)
{
	hyp_put_page(&iommu_pool, vaddr);
}

static struct pkvm_mm_ops viommu_mm_ops = {
	.phys_to_virt = host_gpa2hva,
};

static struct pkvm_mm_ops iommu_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = iommu_zalloc_page,
	.get_page = iommu_get_page,
	.put_page = iommu_put_page,
	.page_count = hyp_page_count,
};

static bool iommu_paging_entry_present(void *ptep)
{
	u64 val;

	val = *(u64 *)ptep;
	return !!(val & 1);
}

static unsigned long iommu_paging_entry_to_phys(void *ptep)
{
	u64 val = *(u64 *)ptep;

	return val & VTD_PAGE_MASK;
}

static int iommu_paging_entry_to_index(unsigned long vaddr, int level)
{
	switch (level) {
	case IOMMU_PASID_TABLE:
		return vaddr & (BIT(PASIDDIR_BITS) - 1);
	case IOMMU_PASID_DIR:
		return (vaddr >> PASIDDIR_SHIFT) & (BIT(PASIDDIR_BITS) - 1);
	case IOMMU_SM_CONTEXT:
		return (vaddr >> DEVFN_SHIFT) & (BIT(SM_DEVFN_BITS) - 1);
	case IOMMU_SM_ROOT:
		return (vaddr >> SM_BUS_SHIFT) & (BIT(SM_BUS_BITS) - 1);
	default:
		break;
	}

	return -EINVAL;
}

static bool iommu_paging_entry_is_leaf(void *ptep, int level)
{
	if (level == IOMMU_PASID_TABLE ||
		!iommu_paging_entry_present(ptep))
		return true;

	return false;
}

static int iommu_paging_level_entry_size(int level)
{
	switch (level) {
	case IOMMU_PASID_TABLE:
		return sizeof(struct pasid_entry);
	case IOMMU_PASID_DIR:
		return sizeof(struct pasid_dir_entry);
	case IOMMU_SM_CONTEXT:
		/* scalable mode requires 32bytes for context */
		return sizeof(struct context_entry) * 2;
	case IOMMU_SM_ROOT:
		return sizeof(u64);
	default:
		break;
	}

	return -EINVAL;
}

static int iommu_paging_level_to_entries(int level)
{
	switch (level) {
	case IOMMU_PASID_TABLE:
		return 1 << PASIDTAB_BITS;
	case IOMMU_PASID_DIR:
		return 1 << PASIDDIR_BITS;
	case IOMMU_SM_CONTEXT:
		return 1 << SM_DEVFN_BITS;
	case IOMMU_SM_ROOT:
		return 1 << SM_BUS_BITS;
	default:
		break;
	}

	return -EINVAL;
}

static unsigned long iommu_paging_level_to_size(int level)
{
	switch (level) {
	case IOMMU_PASID_TABLE:
		return 1;
	case IOMMU_PASID_DIR:
		return 1 << PASIDDIR_SHIFT;
	case IOMMU_SM_CONTEXT:
		return 1 << DEVFN_SHIFT;
	case IOMMU_SM_ROOT:
		return 1 << SM_BUS_SHIFT;
	default:
		break;
	}

	return 0;
}

struct pkvm_pgtable_ops iommu_paging_ops = {
	.pgt_entry_present = iommu_paging_entry_present,
	.pgt_entry_to_phys = iommu_paging_entry_to_phys,
	.pgt_entry_to_index = iommu_paging_entry_to_index,
	.pgt_entry_is_leaf = iommu_paging_entry_is_leaf,
	.pgt_level_entry_size = iommu_paging_level_entry_size,
	.pgt_level_to_entries = iommu_paging_level_to_entries,
	.pgt_level_to_size = iommu_paging_level_to_size,
};

static int initialize_iommu_pgt(struct pkvm_iommu *iommu)
{
	struct pkvm_pgtable *pgt = &iommu->pgt;
	struct pkvm_pgtable *vpgt = &iommu->viommu.pgt;
	struct pkvm_pgtable_cap cap;
	u64 grt_pa = readq(iommu->iommu.reg + DMAR_RTADDR_REG) & VTD_PAGE_MASK;
	int ret;

	cap.level = IOMMU_SM_ROOT;

	vpgt->root_pa = grt_pa;
	ret = pkvm_pgtable_init(vpgt, &viommu_mm_ops, &iommu_paging_ops, &cap, false);
	if (ret)
		return ret;

	ret = pkvm_pgtable_init(pgt, &iommu_mm_ops, &iommu_paging_ops, &cap, true);
	if (!ret) {
		/*
		 * Hold additional reference count to make
		 * sure root page won't be freed
		 */
		void *root = pgt->mm_ops->phys_to_virt(pgt->root_pa);

		pgt->mm_ops->get_page(root);
	}
	return ret;
}

int pkvm_init_iommu(unsigned long mem_base, unsigned long nr_pages)
{
	struct pkvm_iommu_info *info = &pkvm_hyp->iommu_infos[0];
	struct pkvm_iommu *piommu = &iommus[0];
	int i, ret = hyp_pool_init(&iommu_pool, mem_base >> PAGE_SHIFT, nr_pages, 0);

	if (ret)
		return ret;

	for (i = 0; i < PKVM_MAX_IOMMU_NUM; piommu++, info++, i++) {
		if (!info->reg_phys)
			break;

		pkvm_spin_lock_init(&piommu->lock);
		piommu->iommu.reg_phys = info->reg_phys;
		piommu->iommu.reg_size = info->reg_size;
		piommu->iommu.reg = pkvm_iophys_to_virt(info->reg_phys);
		if ((unsigned long)piommu->iommu.reg == INVALID_ADDR)
			return -ENOMEM;
		piommu->iommu.seq_id = i;

		ret = pkvm_mmu_map((unsigned long)piommu->iommu.reg,
				   (unsigned long)info->reg_phys,
				   info->reg_size, 1 << PG_LEVEL_4K,
				   PKVM_PAGE_IO_NOCACHE);
		if (ret)
			return ret;

		piommu->iommu.cap = readq(piommu->iommu.reg + DMAR_CAP_REG);
		piommu->iommu.ecap = readq(piommu->iommu.reg + DMAR_ECAP_REG);

		ret = pkvm_host_ept_unmap((unsigned long)info->reg_phys,
				     (unsigned long)info->reg_phys,
				     info->reg_size);
		if (ret)
			return ret;

		ret = initialize_iommu_pgt(piommu);
		if (ret)
			return ret;
	}

	return 0;
}

static struct pkvm_iommu *find_iommu_by_reg_phys(unsigned long phys)
{
	struct pkvm_iommu *iommu;

	for_each_valid_iommu(iommu) {
		if ((phys >= iommu->iommu.reg_phys) &&
			(phys < (iommu->iommu.reg_phys + iommu->iommu.reg_size)))
			return iommu;
	}

	return NULL;
}

static unsigned long direct_access_iommu_mmio(struct pkvm_iommu *iommu,
					      bool is_read, int len,
					      unsigned long phys,
					      unsigned long val)
{
	unsigned long offset = phys - iommu->iommu.reg_phys;
	void *reg = iommu->iommu.reg + offset;
	unsigned long ret = 0;

	switch (len) {
	case 4:
		if (is_read)
			ret = (unsigned long)readl(reg);
		else
			writel((u32)val, reg);
		break;
	case 8:
		if (is_read)
			ret = (unsigned long)readq(reg);
		else
			writeq((u64)val, reg);
		break;
	default:
		pkvm_err("%s: %s: unsupported len %d\n", __func__,
			 is_read ? "read" : "write", len);
		break;
	}

	return ret;
}

static unsigned long access_iommu_mmio(struct pkvm_iommu *iommu, bool is_read,
				       int len, unsigned long phys,
				       unsigned long val)
{
	unsigned long offset = phys - iommu->iommu.reg_phys;
	unsigned long ret = 0;

	/* pkvm IOMMU driver is not activated yet, so directly access MMIO */
	if (unlikely(!iommu->activated))
		return direct_access_iommu_mmio(iommu, is_read, len, phys, val);

	/* Only need to emulate part of the MMIO */
	switch (offset) {
	default:
		/* Not emulated MMIO can directly goes to hardware */
		ret = direct_access_iommu_mmio(iommu, is_read, len, phys, val);
		break;
	}

	return ret;
}

unsigned long pkvm_access_iommu(bool is_read, int len, unsigned long phys, unsigned long val)
{
	struct pkvm_iommu *pkvm_iommu = find_iommu_by_reg_phys(phys);
	unsigned long ret;

	if (!pkvm_iommu) {
		pkvm_err("%s: cannot find pkvm iommu for reg 0x%lx\n",
			__func__, phys);
		return 0;
	}

	pkvm_spin_lock(&pkvm_iommu->lock);
	ret = access_iommu_mmio(pkvm_iommu, is_read, len, phys, val);
	pkvm_spin_unlock(&pkvm_iommu->lock);

	return ret;
}

bool is_mem_range_overlap_iommu(unsigned long start, unsigned long end)
{
	struct pkvm_iommu *iommu;

	for_each_valid_iommu(iommu) {
		if (end < iommu->iommu.reg_phys ||
			start > (iommu->iommu.reg_phys + iommu->iommu.reg_size - 1))
			continue;

		return true;
	}

	return false;
}
