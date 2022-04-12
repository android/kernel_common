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

/*
 * Guest page table walking parameter.
 * pkvm IOMMU driver walks the guest page table when syncing
 * with the shadow page table.
 */
struct pgt_sync_walk_data {
	struct pkvm_iommu *iommu;
	/*
	 * Used to hold shadow page table physical address
	 * which is used for sync shadow entries at each
	 * page table level.
	 */
	u64 shadow_pa[IOMMU_SM_LEVEL_NUM];
	/*
	 * Used when just syncing a part of shadow
	 * page table entries which match with this did if
	 * it is set as a non-zero did value.
	 */
	u16 did;
};

#define DEFINE_PGT_SYNC_WALK_DATA(name, iommu, domain_id)	\
	struct pgt_sync_walk_data name = {			\
		.iommu = iommu,					\
		.shadow_pa = {0},				\
		.did = (domain_id),				\
	}

/*
 * Used to config a shadow page table entry in root/context/pasid
 * level.
 */
struct pgt_sync_data {
	union {
		u64 root_entry;
		struct context_entry ct_entry;
		struct pasid_dir_entry pd_entry;
		struct pasid_entry p_entry;
	};
	void *guest_ptep;
	void *shadow_ptep;
	int level;
	u64 iommu_ecap;
	u64 shadow_pa;
	struct pkvm_pgtable *spgt;
};

static inline void *iommu_zalloc_pages(size_t size)
{
	return hyp_alloc_pages(&iommu_pool, get_order(size));
}

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

static void iommu_flush_cache(void *ptep, unsigned int size)
{
	pkvm_clflush_cache_range(ptep, size);
}

static struct pkvm_mm_ops viommu_mm_ops = {
	.phys_to_virt = host_gpa2hva,
};

static struct pkvm_mm_ops iommu_pw_coherency_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = iommu_zalloc_page,
	.get_page = iommu_get_page,
	.put_page = iommu_put_page,
	.page_count = hyp_page_count,
};

static struct pkvm_mm_ops iommu_pw_noncoherency_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = iommu_zalloc_page,
	.get_page = iommu_get_page,
	.put_page = iommu_put_page,
	.page_count = hyp_page_count,
	.flush_cache = iommu_flush_cache,
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

static int iommu_pgtable_walk(struct pkvm_pgtable *pgt, unsigned long vaddr,
		       unsigned long vaddr_end, struct pkvm_pgtable_walker *walker)
{
	if (!pgt->root_pa)
		return 0;

	return pgtable_walk(pgt, vaddr, vaddr_end - vaddr, false, walker);
}

/* present root entry when shadow_pa valid, otherwise un-present it */
static bool sync_root_entry(struct pgt_sync_data *sdata)
{
	u64 *sre = sdata->shadow_ptep;
	u64 sre_val = sdata->shadow_pa ? (sdata->shadow_pa | 1) : 0;

	if (READ_ONCE(*sre) != sre_val) {
		WRITE_ONCE(*sre, sre_val);
		return true;
	}

	return false;
}

/* sync context entry when guest_ptep & shadow_pa valid, otherwise un-present it */
static bool sync_shadow_context_entry(struct pgt_sync_data *sdata)
{
	struct context_entry *shadow_ce = sdata->shadow_ptep, tmp = {0};
	bool updated = false;

	if (sdata->guest_ptep && sdata->shadow_pa) {
		struct context_entry *guest_ce = sdata->guest_ptep;

		tmp.hi = guest_ce->hi;
		tmp.lo = sdata->shadow_pa | (guest_ce->lo & 0xfff);
	}

	if (READ_ONCE(shadow_ce->hi) != tmp.hi) {
		WRITE_ONCE(shadow_ce->hi, tmp.hi);
		updated = true;
	}

	if (READ_ONCE(shadow_ce->lo) != tmp.lo) {
		WRITE_ONCE(shadow_ce->lo, tmp.lo);
		updated = true;
	}

	return updated;
}

/* sync pasid dir entry when guest_ptep & shadow_pa valid, otherwise un-present it */
static bool sync_shadow_pasid_dir_entry(struct pgt_sync_data *sdata)
{
	struct pasid_dir_entry *shadow_pde = sdata->shadow_ptep;
	u64 val = 0;

	if (sdata->guest_ptep && sdata->shadow_pa) {
		struct pasid_dir_entry *guest_pde = sdata->guest_ptep;

		val = guest_pde->val & (PASID_PTE_FPD | PASID_PTE_PRESENT);
		val |= sdata->shadow_pa;
	}

	if (READ_ONCE(shadow_pde->val) != val) {
		WRITE_ONCE(shadow_pde->val, val);
		return true;
	}

	return false;
}

/* sync pasid table entry when guest_ptep valid, otherwise un-present it */
static bool sync_shadow_pasid_table_entry(struct pgt_sync_data *sdata)
{
	struct pasid_entry *shadow_pte = sdata->shadow_ptep, tmp_pte = {0};
	struct pasid_entry *guest_pte;
	u64 type, aw;

	if (!sdata->guest_ptep) {
		if (pasid_pte_is_present(shadow_pte)) {
			pasid_clear_entry(shadow_pte);
			return true;
		}

		return false;
	}

	guest_pte = sdata->guest_ptep;
	type = pasid_pte_get_pgtt(guest_pte);
	if (type == PASID_ENTRY_PGTT_FL_ONLY)
		/*
		 * When host IOMMU driver is using first-level only
		 * translation, pkvm IOMMU will actually use nested
		 * translation to add one more layer translation to
		 * guarantee the protection. This one more layer is the
		 * EPT.
		 */
		type = PASID_ENTRY_PGTT_NESTED;
	else if (type == PASID_ENTRY_PGTT_PT)
		/*
		 * When host IOMMU driver is using pass-through mode, pkvm
		 * IOMMU will actually use the second-level only translation
		 * to guarantee the protection. This second-level is als
		 * the EPT.
		 */
		type = PASID_ENTRY_PGTT_SL_ONLY;
	else {
		/*
		 * As the host IOMMU driver in the pkvm enabled kernel has
		 * already been configured to use first-level only or
		 * pass-through mode, it will not to use any other mode. But
		 * in case this happened, just clear the shadow entry and not
		 * to support it.
		 */
		pkvm_err("pkvm: unsupported pasid type %lld\n", type);
		pasid_clear_entry(shadow_pte);
		return true;
	}

	memcpy(&tmp_pte, guest_pte, sizeof(struct pasid_entry));

	pasid_set_page_snoop(&tmp_pte, !!ecap_smpwc(sdata->iommu_ecap));
	if (ecap_sc_support(sdata->iommu_ecap))
		pasid_set_pgsnp(&tmp_pte);

	/*
	 * Modify the second-level related bits:
	 * Set PGTT/SLPTR/AW.
	 * Clear SLADE/SLEE
	 * Reuse FPD/P
	 */
	pasid_set_translation_type(&tmp_pte, type);
	pasid_set_slptr(&tmp_pte, sdata->shadow_pa);
	aw = (pkvm_hyp->ept_iommu_pgt_level == 4) ? 2 : 3;
	pasid_set_address_width(&tmp_pte, aw);
	pasid_set_ssade(&tmp_pte, 0);
	pasid_set_ssee(&tmp_pte, 0);

	return pasid_copy_entry(shadow_pte, &tmp_pte);
}

static bool iommu_paging_sync_entry(struct pgt_sync_data *sdata)
{
	bool ret = false;
	struct pkvm_pgtable *spgt = sdata->spgt;

	switch (sdata->level) {
	case IOMMU_PASID_TABLE:
		ret = sync_shadow_pasid_table_entry(sdata);
		break;
	case IOMMU_PASID_DIR:
		ret = sync_shadow_pasid_dir_entry(sdata);
		break;
	case IOMMU_SM_CONTEXT:
		ret = sync_shadow_context_entry(sdata);
		break;
	case IOMMU_SM_ROOT:
		ret = sync_root_entry(sdata);
		break;
	default:
		break;
	}

	if (ret) {
		int entry_size = spgt->pgt_ops->pgt_level_entry_size(sdata->level);

		if (entry_size && spgt->mm_ops->flush_cache)
			spgt->mm_ops->flush_cache(sdata->shadow_ptep, entry_size);
	}

	return ret;
}

static int initialize_iommu_pgt(struct pkvm_iommu *iommu)
{
	struct pkvm_pgtable *pgt = &iommu->pgt;
	struct pkvm_pgtable *vpgt = &iommu->viommu.pgt;
	static struct pkvm_mm_ops *iommu_mm_ops;
	struct pkvm_pgtable_cap cap;
	u64 grt_pa = readq(iommu->iommu.reg + DMAR_RTADDR_REG) & VTD_PAGE_MASK;
	int ret;

	cap.level = IOMMU_SM_ROOT;

	vpgt->root_pa = grt_pa;
	ret = pkvm_pgtable_init(vpgt, &viommu_mm_ops, &iommu_paging_ops, &cap, false);
	if (ret)
		return ret;

	/*
	 * For the IOMMU without Page-Walk Coherency, should use
	 * iommu_pw_noncoherency_mm_ops to flush CPU cache when
	 * modifying any remapping structure entry.
	 *
	 * For the IOMMU with Page-Walk Coherency, can use
	 * iommu_pw_coherency_mm_ops to skip the CPU cache flushing.
	 */
	if (!ecap_coherent(iommu->iommu.ecap))
		iommu_mm_ops = &iommu_pw_noncoherency_mm_ops;
	else
		iommu_mm_ops = &iommu_pw_coherency_mm_ops;

	ret = pkvm_pgtable_init(pgt, iommu_mm_ops, &iommu_paging_ops, &cap, true);
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
		/* cache the enabled features from Global Status register */
		piommu->iommu.gcmd = readl(piommu->iommu.reg + DMAR_GSTS_REG) &
				     DMAR_GSTS_EN_BITS;

		ret = pkvm_host_ept_unmap((unsigned long)info->reg_phys,
				     (unsigned long)info->reg_phys,
				     info->reg_size);
		if (ret)
			return ret;
	}

	return 0;
}

static int free_shadow_cb(struct pkvm_pgtable *pgt, unsigned long vaddr,
			  unsigned long vaddr_end, int level, void *ptep,
			  unsigned long flags, struct pgt_flush_data *flush_data,
			  void *const arg)
{
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	struct pkvm_mm_ops *mm_ops = pgt->mm_ops;
	struct pgt_sync_data sync_data = {0};
	void *child_ptep;

	/* Doesn't need to do anything if the shadow entry is not present */
	if (!pgt_ops->pgt_entry_present(ptep))
		return 0;

	sync_data.shadow_ptep = ptep;
	sync_data.level = level;
	sync_data.spgt = pgt;

	/* Un-present a present PASID Table entry */
	if (level == IOMMU_PASID_TABLE) {
		iommu_paging_sync_entry(&sync_data);
		mm_ops->put_page(ptep);
		return 0;
	}

	/*
	 * it's a present entry for PASID DIR, context or root.
	 * its child ptep shall already be freed (the refcnt == 1), if so, we
	 * can un-present itself as well now.
	 */
	child_ptep = mm_ops->phys_to_virt(pgt_ops->pgt_entry_to_phys(ptep));
	if (mm_ops->page_count(child_ptep) == 1) {
		iommu_paging_sync_entry(&sync_data);
		mm_ops->put_page(ptep);
		mm_ops->put_page(child_ptep);
	}

	return 0;
}

/* sync_data != NULL, data != NULL */
static int init_sync_data(struct pgt_sync_data *sync_data,
		struct pgt_sync_walk_data *data,
		struct pkvm_iommu *iommu, void *guest_ptep,
		unsigned long vaddr, int level)
{
	struct pkvm_pgtable *spgt = &iommu->pgt;
	int idx = spgt->pgt_ops->pgt_entry_to_index(vaddr, level);
	int entry_size = spgt->pgt_ops->pgt_level_entry_size(level);

	switch (level) {
	case IOMMU_PASID_TABLE:
		sync_data->p_entry = *((struct pasid_entry *)guest_ptep);
		sync_data->guest_ptep = &sync_data->p_entry;
		break;
	case IOMMU_PASID_DIR:
		sync_data->pd_entry = *((struct pasid_dir_entry *)guest_ptep);
		sync_data->guest_ptep = &sync_data->pd_entry;
		break;
	case IOMMU_SM_CONTEXT:
		sync_data->ct_entry = *((struct context_entry *)guest_ptep);
		sync_data->guest_ptep = &sync_data->ct_entry;
		break;
	case IOMMU_SM_ROOT:
		sync_data->root_entry = *((u64 *)guest_ptep);
		sync_data->guest_ptep = &sync_data->root_entry;
		break;
	default:
		return -EINVAL;
	}

	/* shadow_pa of current level must be there */
	if (!data->shadow_pa[level])
		return -EINVAL;

	/* get current shadow_ptep */
	sync_data->shadow_ptep = spgt->mm_ops->phys_to_virt(data->shadow_pa[level]);
	sync_data->shadow_ptep += idx * entry_size;

	sync_data->level = level;
	sync_data->spgt = spgt;
	sync_data->iommu_ecap = iommu->iommu.ecap;
	sync_data->shadow_pa = 0;

	return 0;
}

static int free_shadow(struct pkvm_iommu *iommu, unsigned long vaddr,
		       unsigned long vaddr_end);
static int sync_shadow_cb(struct pkvm_pgtable *vpgt, unsigned long vaddr,
			  unsigned long vaddr_end, int level, void *ptep,
			  unsigned long flags, struct pgt_flush_data *flush_data,
			  void *const arg)
{
	struct pkvm_pgtable_ops *vpgt_ops = vpgt->pgt_ops;
	struct pgt_sync_walk_data *data = arg;
	struct pkvm_iommu *iommu = data->iommu;
	struct pkvm_pgtable *spgt = &iommu->pgt;
	struct pgt_sync_data sync_data;
	void *shadow_ptep, *guest_ptep;
	bool shadow_p, guest_p;
	int ret = init_sync_data(&sync_data, data, iommu, ptep, vaddr, level);

	if (ret < 0)
		return ret;

	guest_ptep = sync_data.guest_ptep;
	shadow_ptep = sync_data.shadow_ptep;

	/*
	 * WALK_TABLE_PRE is for non leaf, WALK_LEAF is for leaf
	 * if not match, it means guest changed it, return -EAGAIN
	 * to re-walk the page table.
	 */
	if ((flags == PKVM_PGTABLE_WALK_TABLE_PRE &&
		vpgt_ops->pgt_entry_is_leaf(guest_ptep, level)) ||
		(flags == PKVM_PGTABLE_WALK_LEAF &&
		!vpgt_ops->pgt_entry_is_leaf(guest_ptep, level)))
		return -EAGAIN;

	shadow_p = spgt->pgt_ops->pgt_entry_present(shadow_ptep);
	guest_p = vpgt_ops->pgt_entry_present(guest_ptep);
	if (!guest_p) {
		if (shadow_p) {
			/*
			 * For the case that guest not present but shadow present, just
			 * simply free the shadow to make them consistent.
			 */
			unsigned long new_vaddr_end = spgt->pgt_ops->pgt_level_to_size(level) +
						      vaddr;
			/*
			 * Get a reference count before free to make sure the current page
			 * of this level and the pages of its parent levels won't be freed.
			 * As here we only want to free its specific sub-level.
			 */
			spgt->mm_ops->get_page(shadow_ptep);
			free_shadow(iommu, vaddr, new_vaddr_end);
			spgt->mm_ops->put_page(shadow_ptep);
		}
		/*
		 * As now both guest and shadow are not
		 * present, don't need to do anything more.
		 */
		return ret;
	}

	if (level == IOMMU_PASID_TABLE) {
		/*
		 * For PASID_TABLE, cache invalidation may want to
		 * sync specific PASID with did matched. So do the
		 * check before sync the entry.
		 *
		 * According to vt-d spec 6.2.2.1, software must not
		 * use domain-id value of 0 on when programming
		 * context-entries on implementations reporting CM=1
		 * in the Capability register.
		 *
		 * So non-zero DID means a real DID from host software.
		 */
		if (data->did && (pasid_get_domain_id(guest_ptep) != data->did))
			return ret;

		/*
		 * The PASID table entry always require to use EPT
		 * for the second-level translation no matter in nested
		 * transltion mode or second-level only mode. So get the
		 * EPT physical address for the leaf entry, which is the
		 * pasid table entry.
		 */
		sync_data.shadow_pa = pkvm_hyp->host_vm.ept->root_pa;
	} else if (!shadow_p) {
		/*
		 * For a non-present non-leaf (which may be root/context/pasid
		 * dir) entry, needs to allocate a new page to make this entry
		 * present. Root and context page are always one page with 4K
		 * size. As we fixed the pasid only support 15bits, which makes
		 * the pasid dir is also one page with 4K size.
		 */
		void *shadow = spgt->mm_ops->zalloc_page();

		if (!shadow)
			return -ENOMEM;
		/* Get the shadow page physical address of the child level */
		sync_data.shadow_pa = spgt->mm_ops->virt_to_phys(shadow);
	} else
		/*
		 * For a present non-leaf (which is probably root/context/pasid dir)
		 * entry, get the shadow page physical address of its child level.
		 */
		sync_data.shadow_pa = spgt->pgt_ops->pgt_entry_to_phys(shadow_ptep);

	if (iommu_paging_sync_entry(&sync_data)) {
		if (!shadow_p)
			/*
			 * A non-present to present changing require to get
			 * a new reference count for the shadow page.
			 */
			spgt->mm_ops->get_page(shadow_ptep);
	}

	if ((flags == PKVM_PGTABLE_WALK_TABLE_PRE) && (level > IOMMU_PASID_TABLE)) {
		/*
		 * As guest page table walking will go to the child level, pass
		 * the shadow page physical address of the child level to sync.
		 */
		data->shadow_pa[level - 1] = sync_data.shadow_pa;
	}

	return ret;
}

static int free_shadow(struct pkvm_iommu *iommu, unsigned long vaddr,
		       unsigned long vaddr_end)
{
	struct pkvm_pgtable_walker walker = {
		.cb = free_shadow_cb,
		.flags = PKVM_PGTABLE_WALK_LEAF |
			 PKVM_PGTABLE_WALK_TABLE_POST,
	};

	/*
	 * To free the shadow IOMMU page table, walks the shadow IOMMU
	 * page table.
	 */
	return iommu_pgtable_walk(&iommu->pgt, vaddr, vaddr_end, &walker);
}

static int sync_shadow(struct pkvm_iommu *iommu, unsigned long vaddr,
		       unsigned long vaddr_end, u16 did)
{
	DEFINE_PGT_SYNC_WALK_DATA(arg, iommu, did);
	struct pkvm_pgtable_walker walker = {
		.cb = sync_shadow_cb,
		.flags = PKVM_PGTABLE_WALK_TABLE_PRE |
			 PKVM_PGTABLE_WALK_LEAF,
		.arg = &arg,
	};
	int ret, retry_cnt = 0;

	if (!iommu->viommu.pgt.root_pa)
		return 0;

retry:
	arg.shadow_pa[IOMMU_SM_ROOT] = iommu->pgt.root_pa;
	/*
	 * To sync the shadow IOMMU page table, walks the guest IOMMU
	 * page table
	 */
	ret = iommu_pgtable_walk(&iommu->viommu.pgt, vaddr, vaddr_end, &walker);
	if ((ret == -EAGAIN) && (retry_cnt++ < 5))
		goto retry;

	return ret;
}

static void enable_qi(struct pkvm_iommu *iommu)
{
	void *desc = iommu->qi.desc;
	int dw, qs;
	u32 sts;

	dw = !!ecap_smts(iommu->iommu.ecap);
	qs = fls(iommu->qi.free_cnt >> (7 + !dw)) - 1;

	/* Disable QI */
	sts = readl(iommu->iommu.reg + DMAR_GSTS_REG);
	if (sts & DMA_GSTS_QIES) {
		iommu->iommu.gcmd &= ~DMA_GCMD_QIE;
		writel(iommu->iommu.gcmd, iommu->iommu.reg + DMAR_GCMD_REG);
		PKVM_IOMMU_WAIT_OP(iommu->iommu.reg + DMAR_GSTS_REG,
				   readl, !(sts & DMA_GSTS_QIES), sts);
	}

	/* Set tail to 0 */
	writel(0, iommu->iommu.reg + DMAR_IQT_REG);

	/* Set IQA */
	iommu->piommu_iqa = pkvm_virt_to_phys(desc) | (dw << 11) | qs;
	writeq(iommu->piommu_iqa, iommu->iommu.reg + DMAR_IQA_REG);

	/* Enable QI */
	iommu->iommu.gcmd |= DMA_GCMD_QIE;
	writel(iommu->iommu.gcmd, iommu->iommu.reg + DMAR_GCMD_REG);
	PKVM_IOMMU_WAIT_OP(iommu->iommu.reg + DMAR_GSTS_REG,
			   readl, (sts & DMA_GSTS_QIES), sts);
}

static int create_qi_desc(struct pkvm_iommu *iommu)
{
	struct pkvm_viommu *viommu = &iommu->viommu;
	struct q_inval *qi = &iommu->qi;
	void __iomem *reg = iommu->iommu.reg;

	pkvm_spin_lock_init(&iommu->qi_lock);
	/*
	 * Before switching the descriptor, need to wait any pending
	 * invalidation descriptor completed. According to spec 6.5.2,
	 * The invalidation queue is considered quiesced when the queue
	 * is empty (head and tail registers equal) and the last
	 * descriptor completed is an Invalidation Wait Descriptor
	 * (which indicates no invalidation requests are pending in hardware).
	 */
	while (readq(reg + DMAR_IQH_REG) !=
		readq(reg + DMAR_IQT_REG))
		cpu_relax();

	viommu->vreg.iqa = viommu->iqa = readq(reg + DMAR_IQA_REG);
	viommu->vreg.iq_head = readq(reg + DMAR_IQH_REG);
	viommu->vreg.iq_tail = readq(reg + DMAR_IQT_REG);

	if (readq(reg + DMAR_GSTS_REG) & DMA_GSTS_QIES) {
		struct qi_desc *wait_desc;
		u64 iqa = viommu->iqa;
		int shift = IQ_DESC_SHIFT(iqa);
		int offset = ((viommu->vreg.iq_head >> shift) +
			      IQ_DESC_LEN(iqa) - 1) % IQ_DESC_LEN(iqa);
		int *desc_status;

		/* Find out the last descriptor */
		wait_desc = pkvm_phys_to_virt(IQ_DESC_BASE_PHYS(iqa)) + (offset << shift);

		pkvm_dbg("pkvm: viommu iqa 0x%llx head 0x%llx tail 0x%llx qw0 0x%llx qw1 0x%llx",
				viommu->vreg.iqa, viommu->vreg.iq_head, viommu->vreg.iq_tail,
				wait_desc->qw0, wait_desc->qw1);

		if (QI_DESC_TYPE(wait_desc->qw0) != QI_IWD_TYPE) {
			pkvm_err("pkvm: %s: expect wait desc but 0x%llx\n",
				 __func__, wait_desc->qw0);
			return -EINVAL;
		}

		desc_status = pkvm_phys_to_virt(wait_desc->qw1);
		/*
		 * Wait until the wait descriptor is completed.
		 *
		 * The desc_status is from host. Checking this in pkvm
		 * is relying on host IOMMU driver won't release the
		 * desc_status after it is completed, and this is guarantee
		 * by the current Linux IOMMU driver.
		 */
		while (READ_ONCE(*desc_status) == QI_IN_USE)
			cpu_relax();
	}

	qi->free_cnt = PKVM_QI_DESC_ALIGNED_SIZE / sizeof(struct qi_desc);
	qi->desc = iommu_zalloc_pages(PKVM_QI_DESC_ALIGNED_SIZE);
	if (!qi->desc)
		return -ENOMEM;

	qi->desc_status = iommu_zalloc_pages(PKVM_QI_DESC_STATUS_ALIGNED_SIZE);
	if (!qi->desc_status) {
		iommu_put_page(qi->desc);
		return -ENOMEM;
	}

	enable_qi(iommu);
	return 0;
}

static int qi_check_fault(struct pkvm_iommu *iommu, int wait_index)
{
	u32 fault;
	struct q_inval *qi = &iommu->qi;

	if (qi->desc_status[wait_index] == QI_ABORT)
		return -EAGAIN;

	fault = readl(iommu->iommu.reg + DMAR_FSTS_REG);

	/*
	 * If IQE happens, the head points to the descriptor associated
	 * with the error. No new descriptors are fetched until the IQE
	 * is cleared.
	 */
	if (fault & DMA_FSTS_IQE) {
		writel(DMA_FSTS_IQE, iommu->iommu.reg + DMAR_FSTS_REG);
		pkvm_dbg("pkvm: Invalidation Queue Error (IQE) cleared\n");
	}

	/*
	 * If ITE happens, all pending wait_desc commands are aborted.
	 * No new descriptors are fetched until the ITE is cleared.
	 */
	if (fault & DMA_FSTS_ITE) {
		writel(DMA_FSTS_ITE, iommu->iommu.reg + DMAR_FSTS_REG);
		pkvm_dbg("pkvm: Invalidation Time-out Error (ITE) cleared\n");
	}

	if (fault & DMA_FSTS_ICE) {
		writel(DMA_FSTS_ICE, iommu->iommu.reg + DMAR_FSTS_REG);
		pkvm_dbg("pkvm: Invalidation Completion Error (ICE) cleared\n");
	}

	return 0;
}

static void submit_qi(struct pkvm_iommu *iommu, struct qi_desc *base, int count)
{
	int len = IQ_DESC_LEN(iommu->piommu_iqa), i, wait_index;
	int shift = IQ_DESC_SHIFT(iommu->piommu_iqa);
	struct q_inval *qi = &iommu->qi;
	struct qi_desc *to, *from;
	int required_cnt = count + 2;
	void *desc = qi->desc;
	int *desc_status, rc;

	pkvm_spin_lock(&iommu->qi_lock);
	/*
	 * Detect if the free descriptor count is enough or not
	 */
	while (qi->free_cnt < required_cnt) {
		u64 head = readq(iommu->iommu.reg + DMAR_IQH_REG) >> shift;
		int busy_cnt = (READ_ONCE(qi->free_head) + len - head) % len;
		int free_cnt = len - busy_cnt;

		if (free_cnt >= required_cnt) {
			qi->free_cnt = free_cnt;
			break;
		}
		pkvm_spin_unlock(&iommu->qi_lock);
		cpu_relax();
		pkvm_spin_lock(&iommu->qi_lock);
	}

	for (i = 0; i < count; i++) {
		from = base + i;
		to = qi->desc + (((qi->free_head + i) % len) << shift);
		to->qw0 = from->qw0;
		to->qw1 = from->qw1;
	}

	wait_index = (qi->free_head + count) % len;
	/* setup wait descriptor */
	to = desc + (wait_index << shift);
	to->qw0 = QI_IWD_STATUS_DATA(QI_DONE) |
		  QI_IWD_STATUS_WRITE | QI_IWD_TYPE;

	desc_status = &qi->desc_status[wait_index];
	WRITE_ONCE(*desc_status, QI_IN_USE);
	to->qw1 = pkvm_virt_to_phys(desc_status);

	/* submit to hardware with wait descriptor */
	qi->free_cnt -= count + 1;
	qi->free_head = (qi->free_head + count + 1) % len;
	writel(qi->free_head << shift, iommu->iommu.reg + DMAR_IQT_REG);

	while (READ_ONCE(*desc_status) != QI_DONE) {
		rc = qi_check_fault(iommu, wait_index);
		if (rc)
			break;
		pkvm_spin_unlock(&iommu->qi_lock);
		cpu_relax();
		pkvm_spin_lock(&iommu->qi_lock);
	}

	if (*desc_status != QI_DONE)
		pkvm_err("pkvm: %s: failed with status %d\n",
			 __func__, *desc_status);

	/* release the free_cnt */
	qi->free_cnt += count + 1;

	pkvm_spin_unlock(&iommu->qi_lock);
}

static void flush_context_cache(struct pkvm_iommu *iommu, u16 did,
				u16 sid, u8 fm, u64 type)
{
	struct qi_desc desc = {.qw1 = 0, .qw2 = 0, .qw3 = 0};

	desc.qw0 = QI_CC_FM(fm) | QI_CC_SID(sid) | QI_CC_DID(did) |
		   QI_CC_GRAN(type) | QI_CC_TYPE;

	submit_qi(iommu, &desc, 1);
}

static void flush_pasid_cache(struct pkvm_iommu *iommu, u16 did,
			      u64 granu, u32 pasid)
{
	struct qi_desc desc = {.qw1 = 0, .qw2 = 0, .qw3 = 0};

	desc.qw0 = QI_PC_PASID(pasid) | QI_PC_DID(did) |
		   QI_PC_GRAN(granu) | QI_PC_TYPE;

	submit_qi(iommu, &desc, 1);
}

static void flush_iotlb(struct pkvm_iommu *iommu, u16 did, u64 addr,
			unsigned int size_order, u64 type)
{
	u8 dw = 0, dr = 0;
	struct qi_desc desc = {.qw2 = 0, .qw3 = 0};
	int ih = 0;

	if (cap_write_drain(iommu->iommu.cap))
		dw = 1;

	if (cap_read_drain(iommu->iommu.cap))
		dr = 1;

	desc.qw0 = QI_IOTLB_DID(did) | QI_IOTLB_DR(dr) | QI_IOTLB_DW(dw) |
		   QI_IOTLB_GRAN(type) | QI_IOTLB_TYPE;
	desc.qw1 = QI_IOTLB_ADDR(addr) | QI_IOTLB_IH(ih) | QI_IOTLB_AM(size_order);

	submit_qi(iommu, &desc, 1);
}

static void set_root_table(struct pkvm_iommu *iommu)
{
	u64 val = iommu->pgt.root_pa;
	void __iomem *reg = iommu->iommu.reg;
	u32 sts;

	/* Set scalable mode */
	val |= DMA_RTADDR_SMT;

	writeq(val, reg + DMAR_RTADDR_REG);

	/*
	 * The shadow root table provides identical remapping results comparing
	 * with the previous guest root table, so it is allowed to switch if
	 * Translation Enable Status is still 1 according to IOMMU spec 6.6:
	 *
	 *  "
	 *  If software sets the root-table pointer while remapping hardware is
	 *  active (TES=1 in Global Status register), software must ensure the
	 *  structures referenced by the new root-table pointer provide identical
	 *  remapping results as the structures referenced by the previous root-table
	 *  pointer so that inflight requests are properly translated.
	 *  "
	 *
	 *  So don't need to turn off TE first before switching.
	 */
	writel(iommu->iommu.gcmd | DMA_GCMD_SRTP, reg + DMAR_GCMD_REG);

	PKVM_IOMMU_WAIT_OP(reg + DMAR_GSTS_REG, readl, (sts & DMA_GSTS_RTPS), sts);

	flush_context_cache(iommu, 0, 0, 0, DMA_CCMD_GLOBAL_INVL);
	flush_pasid_cache(iommu, 0, QI_PC_GLOBAL, 0);
	flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);
}

static int activate_iommu(struct pkvm_iommu *iommu)
{
	unsigned long vaddr = 0, vaddr_end = IOMMU_MAX_VADDR;
	int ret;

	pkvm_dbg("%s: iommu%d\n", __func__, iommu->iommu.seq_id);

	pkvm_spin_lock(&iommu->lock);

	if (!ecap_nest(iommu->iommu.ecap)) {
		ret = -ENODEV;
		goto out;
	}

	ret = initialize_iommu_pgt(iommu);
	if (ret)
		goto out;

	ret = sync_shadow(iommu, vaddr, vaddr_end, 0);
	if (ret)
		goto out;

	ret = create_qi_desc(iommu);
	if (ret)
		goto free_shadow;

	set_root_table(iommu);

	iommu->activated = true;
	root_tbl_walk(iommu);

	pkvm_spin_unlock(&iommu->lock);
	return 0;

free_shadow:
	free_shadow(iommu, vaddr, vaddr_end);
out:
	pkvm_spin_unlock(&iommu->lock);
	return ret;
}

static int handle_descriptor(struct pkvm_iommu *iommu, void *desc)
{
	return 0;
}

static void handle_qi_submit(struct pkvm_iommu *iommu, void *vdesc, int vhead, int count)
{
	struct pkvm_viommu *viommu = &iommu->viommu;
	int vlen = IQ_DESC_LEN(viommu->iqa);
	int vshift = IQ_DESC_SHIFT(viommu->iqa);
	int len = IQ_DESC_LEN(iommu->piommu_iqa);
	int shift = IQ_DESC_SHIFT(iommu->piommu_iqa);
	struct q_inval *qi = &iommu->qi;
	struct qi_desc *to, *from;
	int required_cnt = count + 1, i;

	pkvm_spin_lock(&iommu->qi_lock);
	/*
	 * Detect if the free descriptor count is enough or not
	 */
	while (qi->free_cnt < required_cnt) {
		u64 head = readq(iommu->iommu.reg + DMAR_IQH_REG) >> shift;
		int busy_cnt = (READ_ONCE(qi->free_head) + len - head) % len;
		int free_cnt = len - busy_cnt;

		if (free_cnt >= required_cnt) {
			qi->free_cnt = free_cnt;
			break;
		}
		pkvm_spin_unlock(&iommu->qi_lock);
		cpu_relax();
		pkvm_spin_lock(&iommu->qi_lock);
	}

	for (i = 0; i < count; i++) {
		from = vdesc + (((vhead + i) % vlen) << vshift);
		to = qi->desc + (((qi->free_head + i) % len) << shift);

		to->qw0 = from->qw0;
		to->qw1 = from->qw1;
	}

	/*
	 * Reuse the desc_status from host so that host can poll
	 * the desc_status itself instead of waiting in pkvm.
	 */
	qi->free_cnt -= count;
	qi->free_head = (qi->free_head + count) % len;
	writel(qi->free_head << shift, iommu->iommu.reg + DMAR_IQT_REG);

	pkvm_spin_unlock(&iommu->qi_lock);
}

static int handle_qi_invalidation(struct pkvm_iommu *iommu, unsigned long val)
{
	struct pkvm_viommu *viommu = &iommu->viommu;
	u64 viommu_iqa = viommu->iqa;
	struct qi_desc *wait_desc;
	int len = IQ_DESC_LEN(viommu_iqa);
	int shift = IQ_DESC_SHIFT(viommu_iqa);
	int head = viommu->vreg.iq_head >> shift;
	int count, i, ret = 0;
	int *desc_status;
	void *desc;

	viommu->vreg.iq_tail = val;
	desc = pkvm_phys_to_virt(IQ_DESC_BASE_PHYS(viommu_iqa));
	count = ((val >> shift) + len - head) % len;

	for (i = 0; i < count; i++) {
		viommu->vreg.iq_head = ((head + i) % len) << shift;
		ret = handle_descriptor(iommu, desc + viommu->vreg.iq_head);
		if (ret)
			break;
	}
	/* update iq_head */
	viommu->vreg.iq_head = val;

	if (likely(!ret)) {
		/*
		 * Submit the descriptor to hardware. The desc_status
		 * will be taken cared by hardware.
		 */
		handle_qi_submit(iommu, desc, head, count);
	} else {
		pkvm_err("pkvm: %s: failed with ret %d\n", __func__, ret);
		/*
		 * The descriptor seems invalid. Mark the desc_status as
		 * QI_ABORT to make sure host driver won't be blocked.
		 */
		wait_desc = desc + (((head + count - 1) % len) << shift);
		if (QI_DESC_TYPE(wait_desc->qw0) == QI_IWD_TYPE) {
			desc_status = pkvm_phys_to_virt(wait_desc->qw1);
			WRITE_ONCE(*desc_status, QI_ABORT);
		}
	}

	return ret;
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
	struct pkvm_viommu *viommu = &iommu->viommu;
	unsigned long offset = phys - iommu->iommu.reg_phys;
	unsigned long ret = 0;

	/* pkvm IOMMU driver is not activated yet, so directly access MMIO */
	if (unlikely(!iommu->activated))
		return direct_access_iommu_mmio(iommu, is_read, len, phys, val);

	/* Only need to emulate part of the MMIO */
	switch (offset) {
	case DMAR_IQA_REG:
		if (is_read)
			ret = viommu->vreg.iqa;
		else
			viommu->vreg.iqa = val;
		break;
	case DMAR_IQH_REG:
		if (is_read)
			ret = viommu->vreg.iq_head;
		break;
	case DMAR_IQT_REG:
		if (is_read)
			ret = viommu->vreg.iq_tail;
		else
			ret = handle_qi_invalidation(iommu, val);
		break;
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

int pkvm_activate_iommu(void)
{
	struct pkvm_iommu *iommu;
	int ret = 0;

	for_each_valid_iommu(iommu) {
		ret = activate_iommu(iommu);
		if (ret)
			return ret;
	}

	return 0;
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
