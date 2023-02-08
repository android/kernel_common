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
#include "ptdev.h"
#include "iommu_spgt.h"
#include "bug.h"

#define for_each_valid_iommu(p)						\
	for ((p) = iommus; (p) < iommus + PKVM_MAX_IOMMU_NUM; (p)++)	\
		if (!(p) || !(p)->iommu.reg_phys) {			\
			continue;					\
		} else

static struct pkvm_iommu iommus[PKVM_MAX_IOMMU_NUM];

static struct hyp_pool iommu_pool;

/* Used in legacy mode only. */
struct shadow_pgt_sync_data {
	unsigned long vaddr;
	unsigned long vaddr_end;
};

/*
 * Guest root/context/pasid table (hereinafter "id table") walking parameter.
 * pkvm IOMMU driver walks the guest page table when syncing
 * with the shadow id table.
 */
struct id_sync_walk_data {
	struct pkvm_iommu *iommu;
	/*
	 * Used to hold shadow id table physical address
	 * which is used for sync shadow entries at each
	 * id table level.
	 */
	u64 shadow_pa[IOMMU_SM_LEVEL_NUM];
	/*
	 * Used when just syncing a part of shadow
	 * id table entries which match with this did if
	 * it is set as a non-zero did value.
	 */
	u16 did;
	/*
	 * Used in legacy mode when just syncing a specific
	 * range of pages in shadow page tables.
	 */
	struct shadow_pgt_sync_data *spgt_data;
};

#define DEFINE_ID_SYNC_WALK_DATA(name, _iommu, domain_id, _spgt_data)	\
	struct id_sync_walk_data (name) = {				\
		.iommu = (_iommu),					\
		.shadow_pa = {0},					\
		.did = (domain_id),					\
		.spgt_data = (_spgt_data),				\
	}

/*
 * Used to config a shadow id table entry in root/context/pasid
 * level.
 */
struct id_sync_data {
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
	struct pkvm_pgtable *shadow_id;
	unsigned long vaddr;
	struct shadow_pgt_sync_data *spgt_data;
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

static bool iommu_id_entry_present(void *ptep)
{
	u64 val;

	val = *(u64 *)ptep;
	return !!(val & 1);
}

static unsigned long iommu_id_entry_to_phys(void *ptep)
{
	u64 val = *(u64 *)ptep;

	return val & VTD_PAGE_MASK;
}

static int iommu_sm_id_entry_to_index(unsigned long vaddr, int level)
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

static bool iommu_id_entry_is_leaf(void *ptep, int level)
{
	if (LAST_LEVEL(level) ||
		!iommu_id_entry_present(ptep))
		return true;

	return false;
}

static int iommu_sm_id_level_entry_size(int level)
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

static int iommu_sm_id_level_to_entries(int level)
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

static unsigned long iommu_sm_id_level_to_size(int level)
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

struct pkvm_pgtable_ops iommu_sm_id_ops = {
	.pgt_entry_present = iommu_id_entry_present,
	.pgt_entry_to_phys = iommu_id_entry_to_phys,
	.pgt_entry_to_index = iommu_sm_id_entry_to_index,
	.pgt_entry_is_leaf = iommu_id_entry_is_leaf,
	.pgt_level_entry_size = iommu_sm_id_level_entry_size,
	.pgt_level_to_entries = iommu_sm_id_level_to_entries,
	.pgt_level_to_size = iommu_sm_id_level_to_size,
};

static int iommu_lm_id_entry_to_index(unsigned long vaddr, int level)
{
	switch (level) {
	case IOMMU_LM_CONTEXT:
		return (vaddr >> LM_DEVFN_SHIFT) & (BIT(LM_DEVFN_BITS) - 1);
	case IOMMU_LM_ROOT:
		return (vaddr >> LM_BUS_SHIFT) & (BIT(LM_BUS_BITS) - 1);
	default:
		break;
	}

	return -EINVAL;
}

static int iommu_lm_id_level_entry_size(int level)
{
	switch (level) {
	case IOMMU_LM_CONTEXT:
		return sizeof(struct context_entry);
	case IOMMU_LM_ROOT:
		return sizeof(struct root_entry);
	default:
		break;
	}

	return -EINVAL;
}

static int iommu_lm_id_level_to_entries(int level)
{
	switch (level) {
	case IOMMU_LM_CONTEXT:
		return 1 << LM_DEVFN_BITS;
	case IOMMU_LM_ROOT:
		return 1 << LM_BUS_BITS;
	default:
		break;
	}

	return -EINVAL;
}

static unsigned long iommu_lm_id_level_to_size(int level)
{
	switch (level) {
	case IOMMU_LM_CONTEXT:
		return 1 << LM_DEVFN_SHIFT;
	case IOMMU_LM_ROOT:
		return 1 << LM_BUS_SHIFT;
	default:
		break;
	}

	return 0;
}

struct pkvm_pgtable_ops iommu_lm_id_ops = {
	.pgt_entry_present = iommu_id_entry_present,
	.pgt_entry_to_phys = iommu_id_entry_to_phys,
	.pgt_entry_to_index = iommu_lm_id_entry_to_index,
	.pgt_entry_is_leaf = iommu_id_entry_is_leaf,
	.pgt_level_entry_size = iommu_lm_id_level_entry_size,
	.pgt_level_to_entries = iommu_lm_id_level_to_entries,
	.pgt_level_to_size = iommu_lm_id_level_to_size,
};

static int iommu_pgtable_walk(struct pkvm_pgtable *pgt, unsigned long vaddr,
		       unsigned long vaddr_end, struct pkvm_pgtable_walker *walker)
{
	if (!pgt->root_pa)
		return 0;

	return pgtable_walk(pgt, vaddr, vaddr_end - vaddr, false, walker);
}

static struct pkvm_ptdev *iommu_find_ptdev(struct pkvm_iommu *iommu, u16 bdf, u32 pasid)
{
	struct pkvm_ptdev *p;

	list_for_each_entry(p, &iommu->ptdev_head, iommu_node) {
		if (match_ptdev(p, bdf, pasid))
			return p;
	}

	return NULL;
}

static inline bool iommu_coherency(u64 ecap)
{
	return ecap_smts(ecap) ? ecap_smpwc(ecap) : ecap_coherent(ecap);
}

static struct pkvm_ptdev *iommu_add_ptdev(struct pkvm_iommu *iommu, u16 bdf, u32 pasid)
{
	struct pkvm_ptdev *ptdev = pkvm_get_ptdev(bdf, pasid);

	if (!ptdev) {
		ptdev = pkvm_alloc_ptdev(bdf, pasid, iommu_coherency(iommu->iommu.ecap));
		if (!ptdev)
			return NULL;
	}

	list_add_tail(&ptdev->iommu_node, &iommu->ptdev_head);
	return ptdev;
}

static void iommu_del_ptdev(struct pkvm_iommu *iommu, struct pkvm_ptdev *ptdev)
{
	list_del_init(&ptdev->iommu_node);
	pkvm_put_ptdev(ptdev);
}

static int iommu_audit_did(struct pkvm_iommu *iommu, u16 did, int shadow_vm_handle)
{
	struct pkvm_ptdev *tmp;
	int ret = 0;

	list_for_each_entry(tmp, &iommu->ptdev_head, iommu_node) {
		if (tmp->shadow_vm_handle != shadow_vm_handle) {
			if (tmp->did == did) {
				/*
				 * The devices belong to different VMs but behind
				 * the same IOMMU, cannot use the same did.
				 */
				ret = -EPERM;
				break;
			}
		}
	}

	return ret;
}

static int shadow_pgt_map_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
			       void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	struct pkvm_pgtable_map_data *data = arg;
	unsigned long map_phys;
	int ret = 0;

	host_ept_lock();

	pkvm_host_ept_lookup(data->phys, &map_phys, NULL, NULL);
	if (map_phys == INVALID_ADDR) {
		pkvm_err("pkvm: phys addr 0x%lx not mapped in host ept\n", data->phys);
		goto out;
	}

	ret = pgtable_map_leaf(pgt, vaddr, level, ptep, flush_data, arg);

out:
	host_ept_unlock();
	return ret;
}

/* used in legacy mode only */
static void sync_shadow_pgt(struct pkvm_ptdev *ptdev, struct shadow_pgt_sync_data *sdata)
{
	struct pkvm_pgtable *spgt;
	int ret;

	PKVM_ASSERT(is_pgt_ops_ept(&ptdev->vpgt));

	/*
	 * ptdev->pgt should be already set to this shadow iommu pgtable.
	 * However, ptdev->pgt could change in the meantime due to ptdev
	 * attach to a VM. So to avoid race, do not use ptdev->pgt directly
	 * but get the same shadow iommu pgtable on our own.
	 */
	spgt = pkvm_get_host_iommu_spgt(ptdev->vpgt.root_pa, ptdev->iommu_coherency);
	PKVM_ASSERT(spgt);

	if (sdata)
		ret = pkvm_pgtable_sync_map_range(&ptdev->vpgt, spgt,
						  sdata->vaddr,
						  sdata->vaddr_end - sdata->vaddr,
						  NULL, shadow_pgt_map_leaf);
	else
		ret = pkvm_pgtable_sync_map(&ptdev->vpgt, spgt,
					    NULL, shadow_pgt_map_leaf);
	PKVM_ASSERT(ret == 0);

	pkvm_put_host_iommu_spgt(spgt, ptdev->iommu_coherency);
}

/* present root entry when shadow_pa valid, otherwise un-present it */
static bool sync_root_entry(struct id_sync_data *sdata)
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
static bool sync_shadow_context_entry(struct id_sync_data *sdata)
{
	struct context_entry *shadow_ce = sdata->shadow_ptep, tmp = {0};
	struct context_entry *guest_ce = sdata->guest_ptep;
	struct pkvm_iommu *iommu = pgt_to_pkvm_iommu(sdata->shadow_id);
	struct pkvm_ptdev *ptdev;
	struct pkvm_pgtable_cap cap;
	bool updated = false;
	u8 tt, aw;
	u16 bdf, did;

	if (ecap_smts(sdata->iommu_ecap)) {
		if (sdata->guest_ptep && sdata->shadow_pa) {
			tmp.hi = guest_ce->hi;
			tmp.lo = sdata->shadow_pa | (guest_ce->lo & 0xfff);

			/* Clear DTE to make sure device TLB is disabled for security */
			context_sm_clear_dte(&tmp);
		}
	} else {
		/*
		 * In legacy mode, a context entry is a leaf entry responsible for
		 * configuring the actual address translation for the given ptdev,
		 * much like a PASID table entry in scalable mode. So the below logic
		 * is quite similar to the logic in sync_shadow_pasid_table_entry()
		 * for scalable mode.
		 */
		bdf = sdata->vaddr >> LM_DEVFN_SHIFT;
		ptdev = iommu_find_ptdev(iommu, bdf, 0);

		if (!ptdev) {
			ptdev = iommu_add_ptdev(iommu, bdf, 0);
			if (!ptdev)
				return false;
		}

		if (!sdata->guest_ptep) {
			if (context_lm_is_present(shadow_ce)) {
				pkvm_setup_ptdev_vpgt(ptdev, 0, NULL, NULL, NULL, false);
				pkvm_setup_ptdev_did(ptdev, 0);
				iommu_del_ptdev(iommu, ptdev);

				goto update_shadow_ce;
			}
			return false;
		}

		tt = context_lm_get_tt(guest_ce);
		switch (tt) {
		case CONTEXT_TT_MULTI_LEVEL:
		case CONTEXT_TT_DEV_IOTLB:
			aw = context_lm_get_aw(guest_ce);
			if (aw != 1 && aw != 2 && aw != 3) {
				pkvm_err("pkvm: unsupported address width %u\n", aw);

				pkvm_setup_ptdev_vpgt(ptdev, 0, NULL, NULL, NULL, false);
				pkvm_setup_ptdev_did(ptdev, 0);

				/*
				 * TODO: our error reporting to the host for invalid
				 * values of aw or tt is not good: the host will see
				 * translation fault reason "present bit is clear"
				 * instead of "invalid entry".
				 */
				goto update_shadow_ce;
			}
			cap.level = (aw == 1) ? 3 :
				    (aw == 2) ? 4 : 5;
			cap.allowed_pgsz = pkvm_hyp->ept_cap.allowed_pgsz;
			pkvm_setup_ptdev_vpgt(ptdev, context_lm_get_slptr(guest_ce),
					      &viommu_mm_ops, &ept_ops, &cap, true);

			if (!ptdev_attached_to_vm(ptdev))
				sync_shadow_pgt(ptdev, sdata->spgt_data);

			break;
		case CONTEXT_TT_PASS_THROUGH:
			/*
			 * When host IOMMU driver is using pass-through mode, pkvm
			 * IOMMU will actually use the address translation
			 * (CONTEXT_TT_MULTI_LEVEL) with the primary VM's EPT
			 * to guarantee the protection.
			 */
			break;
		default:
			pkvm_err("pkvm: unsupported translation type %u\n", tt);

			pkvm_setup_ptdev_vpgt(ptdev, 0, NULL, NULL, NULL, false);
			pkvm_setup_ptdev_did(ptdev, 0);
			goto update_shadow_ce;
		}

		did = context_lm_get_did(guest_ce);
		if (iommu_audit_did(iommu, did, ptdev->shadow_vm_handle))
			return false;

		pkvm_setup_ptdev_did(ptdev, did);

		if (!is_pgt_ops_ept(ptdev->pgt))
			return false;

		tmp = *guest_ce;

		/*
		 * Always set translation type to MULTI_LEVEL to ensure address
		 * translation and to disable device TLB for security.
		 */
		context_lm_set_tt(&tmp, CONTEXT_TT_MULTI_LEVEL);
		context_lm_set_slptr(&tmp, ptdev->pgt->root_pa);
		aw = (ptdev->pgt->level == 3) ? 1 :
		     (ptdev->pgt->level == 4) ? 2 : 3;
		context_lm_set_aw(&tmp, aw);
	}

update_shadow_ce:
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
static bool sync_shadow_pasid_dir_entry(struct id_sync_data *sdata)
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
static bool sync_shadow_pasid_table_entry(struct id_sync_data *sdata)
{
	u16 bdf = sdata->vaddr >> DEVFN_SHIFT;
	u32 pasid = sdata->vaddr & ((1UL << MAX_NR_PASID_BITS) - 1);
	struct pkvm_iommu *iommu = pgt_to_pkvm_iommu(sdata->shadow_id);
	struct pkvm_ptdev *ptdev = iommu_find_ptdev(iommu, bdf, pasid);
	struct pasid_entry *shadow_pte = sdata->shadow_ptep, tmp_pte = {0};
	struct pasid_entry *guest_pte;
	bool synced = false;
	u64 type, aw;

	if (!ptdev) {
		ptdev = iommu_add_ptdev(iommu, bdf, pasid);
		if (!ptdev)
			return false;
	}

	if (!sdata->guest_ptep) {
		if (pasid_pte_is_present(shadow_pte)) {
			/*
			 * Making a pasid entry not present needs to remove
			 * the corresponding ptdev from IOMMU. It also means
			 * a ptdev's vpgt/did should be reset as well as
			 * deleting ptdev from this iommu.
			 */
			pkvm_setup_ptdev_vpgt(ptdev, 0, NULL, NULL, NULL, false);
			pkvm_setup_ptdev_did(ptdev, 0);
			iommu_del_ptdev(iommu, ptdev);

			synced = pasid_copy_entry(shadow_pte, &tmp_pte);
		}
		return synced;
	}

	guest_pte = sdata->guest_ptep;
	type = pasid_pte_get_pgtt(guest_pte);
	if (type == PASID_ENTRY_PGTT_FL_ONLY) {
		struct pkvm_pgtable_cap cap;

		if (ptdev_attached_to_vm(ptdev))
			/*
			 * For the attached ptdev, use SL Only mode with
			 * using ptdev->pgt so that the translation is
			 * totally controlled by pkvm.
			 */
			type = PASID_ENTRY_PGTT_SL_ONLY;
		else
			/*
			 * For the other ptdev, pkvm IOMMU will use nested
			 * translation to add one more layer translation to
			 * guarantee the protection. This one more layer is the
			 * primary VM's EPT.
			 */
			type = PASID_ENTRY_PGTT_NESTED;

		/* ptdev vpgt can be initialized with flptr */
		cap.level = pasid_get_flpm(guest_pte) == 0 ? 4 : 5;
		cap.allowed_pgsz = pkvm_hyp->mmu_cap.allowed_pgsz;
		pkvm_setup_ptdev_vpgt(ptdev, pasid_get_flptr(guest_pte),
				      &viommu_mm_ops, &mmu_ops, &cap, false);
	} else if (type == PASID_ENTRY_PGTT_PT) {
		/*
		 * When host IOMMU driver is using pass-through mode, pkvm
		 * IOMMU will actually use the second-level only translation
		 * to guarantee the protection. This second-level is als
		 * the EPT.
		 */
		type = PASID_ENTRY_PGTT_SL_ONLY;
	} else {
		/*
		 * As the host IOMMU driver in the pkvm enabled kernel has
		 * already been configured to use first-level only or
		 * pass-through mode, it will not use any other mode. But
		 * in case this has happened, reset the ptdev vpgt/did while
		 * keep ptdev linked to this IOMMU, and clear the shadow entry
		 * so that not to support it.
		 */
		pkvm_setup_ptdev_vpgt(ptdev, 0, NULL, NULL, NULL, false);
		pkvm_setup_ptdev_did(ptdev, 0);

		pkvm_err("pkvm: unsupported pasid type %lld\n", type);

		return pasid_copy_entry(shadow_pte, &tmp_pte);
	}

	pkvm_setup_ptdev_did(ptdev, pasid_get_domain_id(guest_pte));

	if (iommu_audit_did(iommu, ptdev->did, ptdev->shadow_vm_handle))
		/*
		 * It is possible that this ptdev will be attached to a protected
		 * VM so primary VM allocates the same did used by this protected
		 * VM and did a TLB flush. But at this moment, this ptdev is not
		 * attached yet so audit is failed. For this case, can skip the sync
		 * of this pasid table entry and it will be synced again when this
		 * ptdev is attached.
		 *
		 * It is also possible that this ptdev is just detached from a
		 * protected VM but still using the previous did due to primary VM
		 * has not configured this ptdev yet. In this case, the did of this
		 * ptdev is still the same as the did used by other ptdevs not
		 * detached yet. For this case, can skip the sync of this pasid
		 * table entry and it will be synced again when primary VM configures
		 * this ptdev.
		 *
		 * If not the above cases but primary VM does this by purpose, also
		 * not sync the pasid table entry to guarantee the isolation.
		 */
		return false;

	/*
	 * ptdev->pgt will be used as second-level translation table
	 * which should be EPT format.
	 */
	if (!is_pgt_ops_ept(ptdev->pgt))
		return false;

	/*
	 * Copy all the bits from guest_pte. As the translation type will
	 * be re-configured in below, even some bits inherit from guest_pte
	 * but hardware will ignore those bits according to the translation
	 * type.
	 */
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
	pasid_set_slptr(&tmp_pte, ptdev->pgt->root_pa);
	aw = (ptdev->pgt->level == 4) ? 2 : 3;
	pasid_set_address_width(&tmp_pte, aw);
	pasid_set_ssade(&tmp_pte, 0);
	pasid_set_ssee(&tmp_pte, 0);

	return pasid_copy_entry(shadow_pte, &tmp_pte);
}

static bool iommu_id_sync_entry(struct id_sync_data *sdata)
{
	bool ret = false;
	struct pkvm_pgtable *shadow_id = sdata->shadow_id;

	if (ecap_smts(sdata->iommu_ecap)) {
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
	} else {
		switch (sdata->level) {
		case IOMMU_LM_CONTEXT:
			ret = sync_shadow_context_entry(sdata);
			break;
		case IOMMU_LM_ROOT:
			ret = sync_root_entry(sdata);
			break;
		default:
			break;
		}
	}

	if (ret) {
		int entry_size = shadow_id->pgt_ops->pgt_level_entry_size(sdata->level);

		if (entry_size && shadow_id->mm_ops->flush_cache)
			shadow_id->mm_ops->flush_cache(sdata->shadow_ptep, entry_size);
	}

	return ret;
}

static int initialize_iommu_pgt(struct pkvm_iommu *iommu)
{
	struct pkvm_pgtable *pgt = &iommu->pgt;
	struct pkvm_pgtable *vpgt = &iommu->viommu.pgt;
	static struct pkvm_mm_ops *iommu_mm_ops;
	struct pkvm_pgtable_ops *iommu_ops;
	struct pkvm_pgtable_cap cap;
	u64 grt_pa = readq(iommu->iommu.reg + DMAR_RTADDR_REG) & VTD_PAGE_MASK;
	int ret;

	if (ecap_smts(iommu->iommu.ecap)) {
		cap.level = IOMMU_SM_ROOT;
		iommu_ops = &iommu_sm_id_ops;
	} else {
		cap.level = IOMMU_LM_ROOT;
		iommu_ops = &iommu_lm_id_ops;
	}

	vpgt->root_pa = grt_pa;
	ret = pkvm_pgtable_init(vpgt, &viommu_mm_ops, iommu_ops, &cap, false);
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

	ret = pkvm_pgtable_init(pgt, iommu_mm_ops, iommu_ops, &cap, true);
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

		INIT_LIST_HEAD(&piommu->ptdev_head);

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

static int free_shadow_id_cb(struct pkvm_pgtable *pgt, unsigned long vaddr,
			  unsigned long vaddr_end, int level, void *ptep,
			  unsigned long flags, struct pgt_flush_data *flush_data,
			  void *const arg)
{
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	struct pkvm_mm_ops *mm_ops = pgt->mm_ops;
	struct id_sync_data sync_data = {0};
	struct pkvm_iommu *iommu = pgt_to_pkvm_iommu(pgt);
	void *child_ptep;

	/* Doesn't need to do anything if the shadow entry is not present */
	if (!pgt_ops->pgt_entry_present(ptep))
		return 0;

	sync_data.shadow_ptep = ptep;
	sync_data.level = level;
	sync_data.shadow_id = pgt;
	sync_data.iommu_ecap = iommu->iommu.ecap;
	sync_data.vaddr = vaddr;

	/* Un-present a present PASID Table entry */
	if (LAST_LEVEL(level)) {
		if (iommu_id_sync_entry(&sync_data))
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
		if (iommu_id_sync_entry(&sync_data)) {
			mm_ops->put_page(ptep);
			mm_ops->put_page(child_ptep);
		}
	}

	return 0;
}

/* sync_data != NULL, data != NULL */
static int init_sync_id_data(struct id_sync_data *sync_data,
		struct id_sync_walk_data *data,
		struct pkvm_iommu *iommu, void *guest_ptep,
		unsigned long vaddr, int level)
{
	struct pkvm_pgtable *shadow_id = &iommu->pgt;
	int idx = shadow_id->pgt_ops->pgt_entry_to_index(vaddr, level);
	int entry_size = shadow_id->pgt_ops->pgt_level_entry_size(level);

	if (ecap_smts(iommu->iommu.ecap)) {
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
	} else {
		switch (level) {
		case IOMMU_LM_CONTEXT:
			sync_data->ct_entry = *((struct context_entry *)guest_ptep);
			sync_data->guest_ptep = &sync_data->ct_entry;
			break;
		case IOMMU_LM_ROOT:
			sync_data->root_entry = *((u64 *)guest_ptep);
			sync_data->guest_ptep = &sync_data->root_entry;
			break;
		default:
			return -EINVAL;
		}
	}

	/* shadow_pa of current level must be there */
	if (!data->shadow_pa[level])
		return -EINVAL;

	/* get current shadow_ptep */
	sync_data->shadow_ptep = shadow_id->mm_ops->phys_to_virt(data->shadow_pa[level]);
	sync_data->shadow_ptep += idx * entry_size;

	sync_data->level = level;
	sync_data->shadow_id = shadow_id;
	sync_data->iommu_ecap = iommu->iommu.ecap;
	sync_data->shadow_pa = 0;
	sync_data->vaddr = vaddr;
	sync_data->spgt_data = data->spgt_data;

	return 0;
}

static int free_shadow_id(struct pkvm_iommu *iommu, unsigned long vaddr,
		       unsigned long vaddr_end);
static int sync_shadow_id_cb(struct pkvm_pgtable *vpgt, unsigned long vaddr,
			  unsigned long vaddr_end, int level, void *ptep,
			  unsigned long flags, struct pgt_flush_data *flush_data,
			  void *const arg)
{
	struct pkvm_pgtable_ops *vpgt_ops = vpgt->pgt_ops;
	struct id_sync_walk_data *data = arg;
	struct pkvm_iommu *iommu = data->iommu;
	struct pkvm_pgtable *shadow_id = &iommu->pgt;
	struct id_sync_data sync_data;
	void *shadow_ptep, *guest_ptep;
	bool shadow_p, guest_p;
	int ret = init_sync_id_data(&sync_data, data, iommu, ptep, vaddr, level);

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

	shadow_p = shadow_id->pgt_ops->pgt_entry_present(shadow_ptep);
	guest_p = vpgt_ops->pgt_entry_present(guest_ptep);
	if (!guest_p) {
		if (shadow_p) {
			/*
			 * For the case that guest not present but shadow present, just
			 * simply free the shadow to make them consistent.
			 */
			unsigned long new_vaddr_end = shadow_id->pgt_ops->pgt_level_to_size(level) +
						      vaddr;
			/*
			 * Get a reference count before free to make sure the current page
			 * of this level and the pages of its parent levels won't be freed.
			 * As here we only want to free its specific sub-level.
			 */
			shadow_id->mm_ops->get_page(shadow_ptep);
			free_shadow_id(iommu, vaddr, new_vaddr_end);
			shadow_id->mm_ops->put_page(shadow_ptep);
		}
		/*
		 * As now both guest and shadow are not
		 * present, don't need to do anything more.
		 */
		return ret;
	}

	if (LAST_LEVEL(level)) {
		/*
		 * Cache invalidation may want to sync specific PASID entries
		 * (in scalable mode) or context entries (in legacy mode) with
		 * DID matched. In such case we only need to sync the entries
		 * with the matching DID.
		 *
		 * According to vt-d spec 6.2.2.1 and 6.2.3.1, software must
		 * not use domain-id value of 0 when programming entries on
		 * implementations reporting CM=1 in the Capability register.
		 * So non-zero DID means a real DID from host software.
		 */
		if (data->did) {
			u16 did = ecap_smts(iommu->iommu.ecap)
				? pasid_get_domain_id(guest_ptep)
				: context_lm_get_did(guest_ptep);

			if (did != data->did)
				return ret;
		}

		/*
		 * For a leaf entry, the physical address of its child level
		 * is determined by the pgt used by the corresponding ptdev.
		 * So no need to set sync_data.shadow_pa.
		 */
	} else if (!shadow_p) {
		/*
		 * For a non-present non-leaf (which may be root/context/pasid
		 * dir) entry, needs to allocate a new page to make this entry
		 * present. Root and context page are always one page with 4K
		 * size. As we fixed the pasid only support 15bits, which makes
		 * the pasid dir is also one page with 4K size.
		 */
		void *shadow = shadow_id->mm_ops->zalloc_page();

		if (!shadow)
			return -ENOMEM;
		/* Get the shadow id physical address of the child level */
		sync_data.shadow_pa = shadow_id->mm_ops->virt_to_phys(shadow);
	} else
		/*
		 * For a present non-leaf (which is probably root/context/pasid dir)
		 * entry, get the shadow id physical address of its child level.
		 */
		sync_data.shadow_pa = shadow_id->pgt_ops->pgt_entry_to_phys(shadow_ptep);

	if (iommu_id_sync_entry(&sync_data)) {
		if (!shadow_p)
			/*
			 * A non-present to present changing require to get
			 * a new reference count for the shadow id page.
			 */
			shadow_id->mm_ops->get_page(shadow_ptep);
	}

	if ((flags == PKVM_PGTABLE_WALK_TABLE_PRE) && (!LAST_LEVEL(level))) {
		/*
		 * As guest page table walking will go to the child level, pass
		 * the shadow id physical address of the child level to sync.
		 */
		data->shadow_pa[level - 1] = sync_data.shadow_pa;
	}

	return ret;
}

static int free_shadow_id(struct pkvm_iommu *iommu, unsigned long vaddr,
		       unsigned long vaddr_end)
{
	struct pkvm_pgtable_walker walker = {
		.cb = free_shadow_id_cb,
		.flags = PKVM_PGTABLE_WALK_LEAF |
			 PKVM_PGTABLE_WALK_TABLE_POST,
	};

	/*
	 * To free the shadow IOMMU page table, walks the shadow IOMMU
	 * page table.
	 */
	if (!(iommu->viommu.vreg.gsts & DMA_GSTS_TES))
		return 0;

	return iommu_pgtable_walk(&iommu->pgt, vaddr, vaddr_end, &walker);
}

static int sync_shadow_id(struct pkvm_iommu *iommu, unsigned long vaddr,
		       unsigned long vaddr_end, u16 did,
		       struct shadow_pgt_sync_data *spgt_data)
{
	DEFINE_ID_SYNC_WALK_DATA(arg, iommu, did, spgt_data);
	struct pkvm_pgtable_walker walker = {
		.cb = sync_shadow_id_cb,
		.flags = PKVM_PGTABLE_WALK_TABLE_PRE |
			 PKVM_PGTABLE_WALK_LEAF,
		.arg = &arg,
	};
	int ret, retry_cnt = 0;

	if (!(iommu->viommu.vreg.gsts & DMA_GSTS_TES))
		return 0;

retry:
	if (ecap_smts(iommu->iommu.ecap))
		arg.shadow_pa[IOMMU_SM_ROOT] = iommu->pgt.root_pa;
	else
		arg.shadow_pa[IOMMU_LM_ROOT] = iommu->pgt.root_pa;
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

	if (viommu->vreg.gsts & DMA_GSTS_QIES) {
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

static void __submit_qi(struct pkvm_iommu *iommu, struct qi_desc *base, int count)
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

static void submit_qi(struct pkvm_iommu *iommu, struct qi_desc *base, int count)
{
	int max_len = IQ_DESC_LEN(iommu->piommu_iqa) - 2;
	int submit_count;

	do {
		submit_count = count > max_len ? max_len : count;
		__submit_qi(iommu, base, submit_count);

		count -= submit_count;
		base += submit_count;
	} while (count > 0);
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

static void setup_iotlb_qi_desc(struct pkvm_iommu *iommu,
				struct qi_desc *desc, u16 did,
				u64 addr, unsigned int size_order,
				u64 type)
{
	u8 dw = 0, dr = 0;

	if (cap_write_drain(iommu->iommu.cap))
		dw = 1;

	if (cap_read_drain(iommu->iommu.cap))
		dr = 1;

	desc->qw0 = QI_IOTLB_DID(did) | QI_IOTLB_DR(dr) | QI_IOTLB_DW(dw) |
		    QI_IOTLB_GRAN(type) | QI_IOTLB_TYPE;
	desc->qw1 = QI_IOTLB_ADDR(addr) | QI_IOTLB_AM(size_order);
	desc->qw2 = 0;
	desc->qw3 = 0;
}

static void flush_iotlb(struct pkvm_iommu *iommu, u16 did, u64 addr,
			unsigned int size_order, u64 type)
{
	struct qi_desc desc;

	setup_iotlb_qi_desc(iommu, &desc, did, addr, size_order, type);
	submit_qi(iommu, &desc, 1);
}

static void set_root_table(struct pkvm_iommu *iommu)
{
	u64 val = iommu->pgt.root_pa;
	void __iomem *reg = iommu->iommu.reg;
	u32 sts;

	/* Set scalable mode */
	if (ecap_smts(iommu->iommu.ecap))
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
	if (ecap_smts(iommu->iommu.ecap))
		flush_pasid_cache(iommu, 0, QI_PC_GLOBAL, 0);
	flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);
}

static void enable_translation(struct pkvm_iommu *iommu)
{
	void __iomem *reg = iommu->iommu.reg;
	u32 sts;

	if (iommu->iommu.gcmd & DMA_GCMD_TE)
		return;

	iommu->iommu.gcmd |= DMA_GCMD_TE;

	writel(iommu->iommu.gcmd, reg + DMAR_GCMD_REG);

	PKVM_IOMMU_WAIT_OP(reg + DMAR_GSTS_REG, readl, (sts & DMA_GSTS_TES), sts);
}

static void initialize_viommu_reg(struct pkvm_iommu *iommu)
{
	struct viommu_reg *vreg = &iommu->viommu.vreg;
	void __iomem *reg_base = iommu->iommu.reg;

	vreg->cap = readq(reg_base + DMAR_CAP_REG);
	vreg->ecap = readq(reg_base + DMAR_ECAP_REG);
	pkvm_update_iommu_virtual_caps(&vreg->cap, &vreg->ecap);

	vreg->gsts = readl(reg_base + DMAR_GSTS_REG);
	vreg->rta = readq(reg_base + DMAR_RTADDR_REG);

	pkvm_dbg("%s: iommu phys reg 0x%llx cap 0x%llx ecap 0x%llx gsts 0x%x rta 0x%llx\n",
		 __func__, iommu->iommu.reg_phys, vreg->cap, vreg->ecap, vreg->gsts, vreg->rta);

	/* Invalidate Queue regs are updated when create descriptor */
}

static int activate_iommu(struct pkvm_iommu *iommu)
{
	unsigned long vaddr = 0, vaddr_end = IOMMU_MAX_VADDR;
	int ret;

	pkvm_dbg("%s: iommu%d\n", __func__, iommu->iommu.seq_id);

	pkvm_spin_lock(&iommu->lock);

	ret = initialize_iommu_pgt(iommu);
	if (ret)
		goto out;

	initialize_viommu_reg(iommu);

	ret = sync_shadow_id(iommu, vaddr, vaddr_end, 0, NULL);
	if (ret)
		goto out;

	ret = create_qi_desc(iommu);
	if (ret)
		goto free_shadow;

	set_root_table(iommu);

	/*
	 * It is possible that some of the IOMMU devices doesn't have memory
	 * remapping translation enabled by the host IOMMU driver during boot
	 * time, so pkvm IOMMU driver needs to make sure enabling this to
	 * guarantee the IO isolation from the devices behind this IOMMU.
	 *
	 */
	enable_translation(iommu);

	iommu->activated = true;
	root_tbl_walk(iommu);

	pkvm_spin_unlock(&iommu->lock);
	return 0;

free_shadow:
	free_shadow_id(iommu, vaddr, vaddr_end);
out:
	pkvm_spin_unlock(&iommu->lock);
	return ret;
}

static int context_cache_invalidate(struct pkvm_iommu *iommu, struct qi_desc *desc)
{
	u16 sid = QI_DESC_CC_SID(desc->qw0);
	u16 did = ecap_smts(iommu->iommu.ecap) ? 0 : QI_DESC_CC_DID(desc->qw0);
	u64 granu = QI_DESC_CC_GRANU(desc->qw0) << DMA_CCMD_INVL_GRANU_OFFSET;
	unsigned long start, end;
	int ret;

	switch (granu) {
	case DMA_CCMD_GLOBAL_INVL:
		start = 0;
		end = MAX_NUM_OF_ADDRESS_SPACE(iommu);
		pkvm_dbg("pkvm: %s: iommu%d: global\n", __func__, iommu->iommu.seq_id);
		ret = sync_shadow_id(iommu, start, end, 0, NULL);
		break;
	case DMA_CCMD_DOMAIN_INVL:
		/*
		 * Domain selective invalidation which is processed by
		 * hardware as global invalidations for scalable mode
		 * according to spec 6.5.2.1
		 */
		start = 0;
		end = MAX_NUM_OF_ADDRESS_SPACE(iommu);
		pkvm_dbg("pkvm: %s: iommu%d: domain selective\n",
			 __func__, iommu->iommu.seq_id);
		ret = sync_shadow_id(iommu, start, end, did, NULL);
		break;
	case DMA_CCMD_DEVICE_INVL:
		if (ecap_smts(iommu->iommu.ecap)) {
			start = (unsigned long)sid << DEVFN_SHIFT;
			end = ((unsigned long)sid + 1) << DEVFN_SHIFT;
		} else {
			start = (unsigned long)sid << LM_DEVFN_SHIFT;
			end = ((unsigned long)sid + 1) << LM_DEVFN_SHIFT;
		}
		pkvm_dbg("pkvm: %s: iommu%d: device selective sid 0x%x\n",
			 __func__, iommu->iommu.seq_id, sid);
		ret = sync_shadow_id(iommu, start, end, did, NULL);
		break;
	default:
		pkvm_err("pkvm: %s: iommu%d: invalidate granu %lld\n",
			__func__, iommu->iommu.seq_id, granu >> DMA_CCMD_INVL_GRANU_OFFSET);
		ret = -EINVAL;
		break;
	}

	if (ret)
		pkvm_err("pkvm: %s: iommu%d: granularity %lld failed with ret %d\n",
			__func__, iommu->iommu.seq_id, granu >> DMA_CCMD_INVL_GRANU_OFFSET, ret);
	return ret;
}

static int pasid_cache_invalidate(struct pkvm_iommu *iommu, struct qi_desc *desc)
{
	int pasid = QI_DESC_PC_PASID(desc->qw0);
	u16 did = QI_DESC_PC_DID(desc->qw0);
	int granu = QI_DESC_PC_GRANU(desc->qw0);
	unsigned long start, end;
	int ret;

	switch (granu) {
	case QI_PC_ALL_PASIDS:
		/*
		 * This is more like a global invalidation but to check
		 * if matching with a specific DID.
		 */
		pkvm_dbg("pkvm: %s: iommu%d: ALL_PASID did %d\n",
			 __func__, iommu->iommu.seq_id, did);
		start = 0;
		end = IOMMU_MAX_VADDR;
		ret = sync_shadow_id(iommu, start, end, did, NULL);
		break;
	case QI_PC_PASID_SEL: {
		/*
		 * Sync specific PASID entry for all contexts
		 */
		u64 bdf, end_bdf = 0x10000;

		pkvm_dbg("pkvm: %s: iommu%d: PASID_SEL did %d pasid 0x%x\n",
			 __func__, iommu->iommu.seq_id, did, pasid);
		for (bdf = 0; bdf < end_bdf; bdf++) {
			start = (bdf << DEVFN_SHIFT) + pasid;
			end = start + 1;
			ret = sync_shadow_id(iommu, start, end, did, NULL);
			if (ret)
				break;
		}
		break;
	}
	case QI_PC_GLOBAL:
		start = 0;
		end = IOMMU_MAX_VADDR;
		pkvm_dbg("pkvm: %s: iommu%d: global\n", __func__, iommu->iommu.seq_id);
		ret = sync_shadow_id(iommu, start, end, 0, NULL);
		break;
	default:
		pkvm_err("pkvm: %s: iommu%d: invalid granularity %d 0x%llx\n",
			 __func__, iommu->iommu.seq_id, granu, desc->qw0);
		ret = -EINVAL;
		break;
	}

	if (ret)
		pkvm_err("pkvm: %s: iommu%d: granularity %d failed with ret %d\n",
			 __func__, iommu->iommu.seq_id, granu, ret);

	return ret;
}

static int iotlb_lm_invalidate(struct pkvm_iommu *iommu, struct qi_desc *desc)
{
	u16 did = QI_DESC_IOTLB_DID(desc->qw0);
	u64 granu = QI_DESC_IOTLB_GRANU(desc->qw0) << DMA_TLB_FLUSH_GRANU_OFFSET;
	u64 addr = QI_DESC_IOTLB_ADDR(desc->qw1);
	u64 mask = ((u64)-1) << (VTD_PAGE_SHIFT + QI_DESC_IOTLB_AM(desc->qw1));
	struct shadow_pgt_sync_data data;
	struct pkvm_ptdev *p;
	int ret;

	switch (granu) {
	case DMA_TLB_GLOBAL_FLUSH:
		pkvm_dbg("pkvm: %s: iommu%d: global\n", __func__, iommu->iommu.seq_id);
		ret = sync_shadow_id(iommu, 0, IOMMU_LM_MAX_VADDR, 0, NULL);
		break;
	case DMA_TLB_DSI_FLUSH:
		pkvm_dbg("pkvm: %s: iommu%d: domain selective did %u\n",
			 __func__, iommu->iommu.seq_id, did);

		/* optimization: walk just the needed devices, not the entire bdf space */
		list_for_each_entry(p, &iommu->ptdev_head, iommu_node)
			if (p->did == did)
				ret = sync_shadow_id(iommu, p->bdf, p->bdf + 1, did, NULL);
		break;
	case DMA_TLB_PSI_FLUSH:
		data.vaddr = addr & mask;
		data.vaddr_end = (addr | ~mask) + 1;
		pkvm_dbg("pkvm: %s: iommu%d: page selective did %u start 0x%lx end 0x%lx\n",
			 __func__, iommu->iommu.seq_id, did, data.vaddr, data.vaddr_end);

		/* optimization: walk just the needed devices, not the entire bdf space */
		list_for_each_entry(p, &iommu->ptdev_head, iommu_node)
			if (p->did == did)
				ret = sync_shadow_id(iommu, p->bdf, p->bdf + 1, did, &data);
		break;
	default:
		pkvm_err("pkvm: %s: iommu%d: invalid granularity %lld\n",
			__func__, iommu->iommu.seq_id, granu >> DMA_TLB_FLUSH_GRANU_OFFSET);
		ret = -EINVAL;
		break;
	}

	if (ret)
		pkvm_err("pkvm: %s: iommu%d: granularity %lld failed with ret %d\n",
			__func__, iommu->iommu.seq_id, granu >> DMA_TLB_FLUSH_GRANU_OFFSET, ret);

	return ret;
}

static int handle_descriptor(struct pkvm_iommu *iommu, struct qi_desc *desc)
{
	int type = QI_DESC_TYPE(desc->qw0);
	int ret = 0;

	switch (type) {
	/*
	 * TODO: is it necessary to intercept the
	 * PGRP_RESP & PSTRM_RESP?
	 */
	case QI_PGRP_RESP_TYPE:
	case QI_PSTRM_RESP_TYPE:
	case QI_DIOTLB_TYPE:
	case QI_DEIOTLB_TYPE:
	case QI_IEC_TYPE:
	case QI_IWD_TYPE:
	case QI_EIOTLB_TYPE:
		break;
	case QI_CC_TYPE:
		ret = context_cache_invalidate(iommu, desc);
		break;
	case QI_PC_TYPE:
		ret = pasid_cache_invalidate(iommu, desc);
		break;
	case QI_IOTLB_TYPE:
		if (!ecap_smts(iommu->iommu.ecap))
			ret = iotlb_lm_invalidate(iommu, desc);
		break;
	default:
		pkvm_err("pkvm: %s: iommu%d: invalid type %d desc addr 0x%llx val 0x%llx\n",
			 __func__, iommu->iommu.seq_id, type, (u64)desc, desc->qw0);
		ret = -EINVAL;
		break;
	}

	return ret;
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

static void handle_gcmd_te(struct pkvm_iommu *iommu, bool en)
{
	unsigned long vaddr = 0, vaddr_end = MAX_NUM_OF_ADDRESS_SPACE(iommu);
	struct pkvm_viommu *viommu = &iommu->viommu;

	if (en) {
		viommu->vreg.gsts |= DMA_GSTS_TES;
		/*
		 * Sync shadow id table to emulate Translation enable.
		 */
		if (sync_shadow_id(iommu, vaddr, vaddr_end, 0, NULL))
			return;
		pkvm_dbg("pkvm: %s: enable TE\n", __func__);
		goto out;
	}

	/*
	 * Free shadow to emulate Translation disable.
	 *
	 * Not really disable translation as still
	 * need to protect agains the device.
	 */
	free_shadow_id(iommu, vaddr, vaddr_end);
	viommu->vreg.gsts &= ~DMA_GSTS_TES;
	pkvm_dbg("pkvm: %s: disable TE\n", __func__);
out:
	flush_context_cache(iommu, 0, 0, 0, DMA_CCMD_GLOBAL_INVL);
	if (ecap_smts(iommu->iommu.ecap))
		flush_pasid_cache(iommu, 0, QI_PC_GLOBAL, 0);
	flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);

	root_tbl_walk(iommu);
}

static void handle_gcmd_srtp(struct pkvm_iommu *iommu)
{
	struct viommu_reg *vreg = &iommu->viommu.vreg;
	struct pkvm_pgtable *vpgt = &iommu->viommu.pgt;

	vreg->gsts &= ~DMA_GSTS_RTPS;

	/* Set the root table phys address from vreg */
	vpgt->root_pa = vreg->rta & VTD_PAGE_MASK;

	pkvm_dbg("pkvm: %s: set SRTP val 0x%llx\n", __func__, vreg->rta);

	if (vreg->gsts & DMA_GSTS_TES) {
		unsigned long vaddr = 0, vaddr_end = MAX_NUM_OF_ADDRESS_SPACE(iommu);

		/* TE is already enabled, sync shadow */
		if (sync_shadow_id(iommu, vaddr, vaddr_end, 0, NULL))
			return;

		flush_context_cache(iommu, 0, 0, 0, DMA_CCMD_GLOBAL_INVL);
		if (ecap_smts(iommu->iommu.ecap))
			flush_pasid_cache(iommu, 0, QI_PC_GLOBAL, 0);
		flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);
	}

	vreg->gsts |= DMA_GSTS_RTPS;

	root_tbl_walk(iommu);
}

static void handle_gcmd_qie(struct pkvm_iommu *iommu, bool en)
{
	struct viommu_reg *vreg = &iommu->viommu.vreg;

	if (en) {
		if (vreg->iq_tail != 0) {
			pkvm_err("pkvm: Queue invalidation descriptor tail is not zero\n");
			return;
		}

		/* Update the iqa from vreg */
		iommu->viommu.iqa = vreg->iqa;
		vreg->iq_head = 0;
		vreg->gsts |= DMA_GSTS_QIES;
		pkvm_dbg("pkvm: %s: enabled QI\n", __func__);
		return;
	}

	if (vreg->iq_head != vreg->iq_tail) {
		pkvm_err("pkvm: Queue invalidation descriptor is not empty yet\n");
		return;
	}

	vreg->iq_head = 0;
	vreg->gsts &= ~DMA_GSTS_QIES;
	pkvm_dbg("pkvm: %s: disabled QI\n", __func__);
}

static void handle_gcmd_direct(struct pkvm_iommu *iommu, u32 val)
{
	struct viommu_reg *vreg = &iommu->viommu.vreg;
	unsigned long changed = ((vreg->gsts ^ val) & DMAR_GCMD_DIRECT) &
				DMAR_GSTS_EN_BITS;
	unsigned long set = (val & DMAR_GCMD_DIRECT) & ~DMAR_GSTS_EN_BITS;
	u32 cmd, gcmd, sts;
	int bit;

	if ((changed | set) & DMAR_GCMD_PROTECTED) {
		pkvm_dbg("pkvm:%s touching protected bits changed 0x%lx set 0x%lx\n",
			 __func__, changed, set);
		return;
	}

	if (changed) {
		pkvm_dbg("pkvm: %s: changed 0x%lx\n", __func__, changed);
		gcmd = READ_ONCE(iommu->iommu.gcmd);
		for_each_set_bit(bit, &changed, BITS_PER_BYTE * sizeof(vreg->gsts)) {
			cmd = 1 << bit;
			if (val & cmd) {
				/* enable */
				gcmd |= cmd;
				writel(gcmd, iommu->iommu.reg + DMAR_GCMD_REG);
				PKVM_IOMMU_WAIT_OP(iommu->iommu.reg + DMAR_GSTS_REG,
						   readl, (sts & cmd), sts);
				vreg->gsts |= cmd;
				pkvm_dbg("pkvm: %s: enable cmd bit %d\n", __func__, bit);
			} else {
				/* disable */
				gcmd &= ~cmd;
				writel(gcmd, iommu->iommu.reg + DMAR_GCMD_REG);
				PKVM_IOMMU_WAIT_OP(iommu->iommu.reg + DMAR_GSTS_REG,
						   readl, !(sts & cmd), sts);
				vreg->gsts &= ~cmd;
				pkvm_dbg("pkvm: %s: disable cmd bit %d\n", __func__, bit);
			}
		}
		WRITE_ONCE(iommu->iommu.gcmd, gcmd);
	}

	if (set) {
		pkvm_dbg("pkvm: %s: set 0x%lx\n", __func__, set);
		gcmd = READ_ONCE(iommu->iommu.gcmd);
		for_each_set_bit(bit, &set, BITS_PER_BYTE * sizeof(vreg->gsts)) {
			cmd = 1 << bit;
			vreg->gsts &= ~cmd;
			writel(gcmd | cmd, iommu->iommu.reg + DMAR_GCMD_REG);
			PKVM_IOMMU_WAIT_OP(iommu->iommu.reg + DMAR_GSTS_REG,
					   readl, (sts & cmd), sts);
			vreg->gsts |= cmd;
			pkvm_dbg("pkvm: %s: set cmd bit %d\n", __func__, bit);
		}
	}
}

static void handle_global_cmd(struct pkvm_iommu *iommu, u32 val)
{
	u32 changed = iommu->viommu.vreg.gsts ^ val;

	pkvm_dbg("pkvm: iommu%d: handle gcmd val 0x%x gsts 0x%x changed 0x%x\n",
		  iommu->iommu.seq_id, val, iommu->viommu.vreg.gsts, changed);

	if (changed & DMA_GCMD_TE)
		handle_gcmd_te(iommu, !!(val & DMA_GCMD_TE));

	if (val & DMA_GCMD_SRTP)
		handle_gcmd_srtp(iommu);

	if (changed & DMA_GCMD_QIE)
		handle_gcmd_qie(iommu, !!(val & DMA_GCMD_QIE));

	handle_gcmd_direct(iommu, val);
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
	case DMAR_CAP_REG:
		if (is_read)
			ret = viommu->vreg.cap;
		break;
	case DMAR_ECAP_REG:
		if (is_read)
			ret = viommu->vreg.ecap;
		break;
	case DMAR_GCMD_REG:
		if (is_read)
			ret = 0;
		else
			handle_global_cmd(iommu, val);
		break;
	case DMAR_GSTS_REG:
		if (is_read)
			ret = viommu->vreg.gsts;
		break;
	case DMAR_RTADDR_REG:
		if (is_read)
			ret = viommu->vreg.rta;
		else
			viommu->vreg.rta = val;
		break;
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
		else {
			if (viommu->vreg.gsts & DMA_GSTS_QIES)
				ret = handle_qi_invalidation(iommu, val);
			else
				viommu->vreg.iq_tail = val;
		}
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

/*
 * TODO:
 * Currently assume that the bdf/pasid has ever been synced
 * so that the IOMMU can be found. If not synced, then cannot
 * get a valid IOMMU by calling this function.
 *
 * To handle this case, pKVM IOMMU driver needs to check the
 * DMAR to know which IOMMU should be used for this bdf/pasid.
 */
static struct pkvm_iommu *bdf_pasid_to_iommu(u16 bdf, u32 pasid)
{
	struct pkvm_iommu *iommu, *find = NULL;
	struct pkvm_ptdev *p;

	for_each_valid_iommu(iommu) {
		pkvm_spin_lock(&iommu->lock);
		list_for_each_entry(p, &iommu->ptdev_head, iommu_node) {
			if (match_ptdev(p, bdf, pasid)) {
				find = iommu;
				break;
			}
		}
		pkvm_spin_unlock(&iommu->lock);
		if (find)
			break;
	}

	return find;
}

/*
 * pkvm_iommu_sync() - Sync IOMMU context/pasid entry according to a ptdev
 *
 * @bdf/pasid:		The corresponding IOMMU page table entry needs to sync.
 */
int pkvm_iommu_sync(u16 bdf, u32 pasid)
{
	struct pkvm_iommu *iommu = bdf_pasid_to_iommu(bdf, pasid);
	unsigned long id_addr, id_addr_end;
	struct pkvm_ptdev *ptdev;
	u16 old_did;
	int ret;

	if (!iommu)
		return -ENODEV;

	ptdev = pkvm_get_ptdev(bdf, pasid);
	if (!ptdev)
		return -ENODEV;

	old_did = ptdev->did;

	if (ecap_smts(iommu->iommu.ecap)) {
		id_addr = ((unsigned long)bdf << DEVFN_SHIFT) |
			  ((unsigned long)pasid & ((1UL << MAX_NR_PASID_BITS) - 1));
		id_addr_end = id_addr + 1;
	} else {
		id_addr = (unsigned long)bdf << LM_DEVFN_SHIFT;
		id_addr_end = ((unsigned long)bdf + 1) << LM_DEVFN_SHIFT;
	}

	pkvm_spin_lock(&iommu->lock);
	ret = sync_shadow_id(iommu, id_addr, id_addr_end, 0, NULL);
	if (!ret) {
		if (old_did != ptdev->did) {
			/* Flush pasid cache and IOTLB for the valid old_did */
			if (ecap_smts(iommu->iommu.ecap))
				flush_pasid_cache(iommu, old_did, QI_PC_PASID_SEL, pasid);
			else
				flush_context_cache(iommu, old_did, 0, 0, DMA_CCMD_DOMAIN_INVL);
			flush_iotlb(iommu, old_did, 0, 0, DMA_TLB_DSI_FLUSH);
		}

		/* Flush pasid cache and IOTLB to make sure no stale TLB for the new did */
		if (ecap_smts(iommu->iommu.ecap))
			flush_pasid_cache(iommu, ptdev->did, QI_PC_PASID_SEL, pasid);
		else
			flush_context_cache(iommu, ptdev->did, 0, 0, DMA_CCMD_DOMAIN_INVL);
		flush_iotlb(iommu, ptdev->did, 0, 0, DMA_TLB_DSI_FLUSH);
	}
	pkvm_spin_unlock(&iommu->lock);

	pkvm_put_ptdev(ptdev);
	return ret;
}

bool pkvm_iommu_coherency(u16 bdf, u32 pasid)
{
	struct pkvm_iommu *iommu = bdf_pasid_to_iommu(bdf, pasid);

	/*
	 * If cannot find a valid IOMMU by bdf/pasid, return
	 * false to present noncoherent, so that can guarantee
	 * the coherency through flushing cache by pkvm itself.
	 */
	if (!iommu)
		return false;

	return iommu_coherency(iommu->iommu.ecap);
}

struct iotlb_flush_data {
	unsigned long desired_root_pa;
	unsigned long addr;
	int size_order;
	struct qi_desc *desc;
	int desc_max_index;
};

static void iommu_flush_iotlb(struct pkvm_iommu *iommu, struct iotlb_flush_data *data)
{
	struct pkvm_ptdev *ptdev;
	struct qi_desc *desc = data->desc;
	int qi_desc_index = 0;

	pkvm_spin_lock(&iommu->lock);

	/* No need to flush IOTLB if there is no device on this IOMMU */
	if (list_empty(&iommu->ptdev_head))
		goto out;

	/*
	 * If the descriptor buffer is NULL, pKVM has to submit the QI
	 * request one by one which may be slow if there are a lot of
	 * devices connected to this IOMMU unit. So in this case, choose
	 * to submit one single global flush request to flush the IOTLB
	 * for all the devices.
	 */
	if (!desc) {
		flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);
		goto out;
	}

	/* Flush per domain */
	list_for_each_entry(ptdev, &iommu->ptdev_head, iommu_node) {
		struct qi_desc *tmp = desc;
		bool did_exist = false;
		int i;

		if (!ptdev->pgt || ptdev->pgt->root_pa != data->desired_root_pa)
			continue;

		for (i = 0; i < qi_desc_index; i++, tmp++) {
			/* The same did is already in descriptor page */
			if (ptdev->did == QI_DESC_IOTLB_DID(tmp->qw0)) {
				did_exist = true;
				break;
			}
		}

		if (did_exist)
			continue;
		/*
		 * Setup the page-selective or domain-selective qi descriptor
		 * based on IOMMU capability, and submit to HW when qi descriptor
		 * number reaches to the maximum count.
		 */
		if (cap_pgsel_inv(iommu->iommu.cap) &&
		    data->size_order <= cap_max_amask_val(iommu->iommu.cap))
			setup_iotlb_qi_desc(iommu, desc + qi_desc_index++,
					    ptdev->did, data->addr, data->size_order,
					    DMA_TLB_PSI_FLUSH);
		else
			setup_iotlb_qi_desc(iommu, desc + qi_desc_index++,
					    ptdev->did, 0, 0,
					    DMA_TLB_DSI_FLUSH);

		if (qi_desc_index == data->desc_max_index) {
			submit_qi(iommu, desc, qi_desc_index);
			qi_desc_index = 0;
		}
	}

	if (qi_desc_index)
		submit_qi(iommu, desc, qi_desc_index);
out:
	pkvm_spin_unlock(&iommu->lock);
}

void pkvm_iommu_flush_iotlb(struct pkvm_pgtable *pgt, unsigned long addr, unsigned long size)
{
	int size_order = ilog2(__roundup_pow_of_two(size >> VTD_PAGE_SHIFT));
	struct iotlb_flush_data data = {
		.desired_root_pa = pgt->root_pa,
		.addr = ALIGN_DOWN(addr, (1ULL << (VTD_PAGE_SHIFT + size_order))),
		.size_order = size_order,
	};
	struct pkvm_iommu *iommu;

	data.desc = iommu_zalloc_pages(PKVM_QI_DESC_ALIGNED_SIZE);
	if (data.desc)
		/* Reserve space for one wait desc and one desc between head and tail */
		data.desc_max_index = PKVM_QI_DESC_ALIGNED_SIZE / sizeof(struct qi_desc) - 2;

	for_each_valid_iommu(iommu)
		iommu_flush_iotlb(iommu, &data);

	if (data.desc)
		iommu_put_page(data.desc);
}
