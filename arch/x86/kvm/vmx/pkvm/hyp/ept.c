// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/types.h>
#include <linux/memblock.h>
#include <asm/kvm_pkvm.h>
#include <asm/pkvm_spinlock.h>
#include <mmu.h>
#include <mmu/spte.h>

#include <pkvm.h>
#include <capabilities.h>

#include "pkvm_hyp.h"
#include "gfp.h"
#include "early_alloc.h"
#include "pgtable.h"
#include "ept.h"
#include "memory.h"
#include "iommu.h"
#include "vmx.h"
#include "mem_protect.h"
#include "debug.h"
#include "ptdev.h"
#include <pkvm/vmx/vmx.h>

static struct hyp_pool host_ept_pool;
static struct pkvm_pgtable host_ept;
static struct pkvm_pgtable host_ept_notlbflush;
static pkvm_spinlock_t _host_ept_lock = __PKVM_SPINLOCK_UNLOCKED;

struct hyp_pool shadow_pgt_pool;

static void *host_ept_zalloc_page(struct pkvm_memcache *mc)
{
	return hyp_alloc_pages(&host_ept_pool, 0);
}

static void host_ept_get_page(void *vaddr)
{
	hyp_get_page(&host_ept_pool, vaddr);
}

static void host_ept_put_page(void *vaddr)
{
	hyp_put_page(&host_ept_pool, vaddr);
}

static void host_ept_flush_tlb(struct pkvm_pgtable *pgt,
			       unsigned long vaddr, unsigned long size)
{
	struct pkvm_host_vcpu *hvcpu;
	int i;

	for (i = 0; i < pkvm_hyp->num_cpus; i++) {
		hvcpu = pkvm_hyp->host_vm.host_vcpus[i];

		kvm_make_request(PKVM_REQ_TLB_FLUSH_HOST_EPT, &hvcpu->vmx.vcpu);
		pkvm_kick_vcpu(&hvcpu->vmx.vcpu);
	}
}

static void host_ept_flush_cache(void *vaddr, unsigned int size)
{
	if (!pkvm_hyp->iommu_coherent)
		pkvm_clflush_cache_range(vaddr, size);
}

const struct pkvm_mm_ops host_ept_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = host_ept_zalloc_page,
	.get_page = host_ept_get_page,
	.put_page = host_ept_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = host_ept_flush_tlb,
	.flush_cache = host_ept_flush_cache,
};

static const struct pkvm_mm_ops host_ept_mm_ops_no_tlbflush = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = host_ept_zalloc_page,
	.get_page = host_ept_get_page,
	.put_page = host_ept_put_page,
	.page_count = hyp_page_count,
	.flush_cache = host_ept_flush_cache,
};

static bool ept_entry_present(void *ptep)
{
	u64 val = *(u64 *)ptep;

	return !!(val & VMX_EPT_RWX_MASK);
}

static bool ept_entry_mapped(void *ptep)
{
	/*
	 * Both present and non-present (shadow)EPT entry is counted as a
	 * mapped entry because a non-present entry with non-zero value may
	 * contain page state and ownership information created through map
	 * operation. So simply count non-zero entry as mapped to cover both
	 * cases.
	 */
	return !!(*(u64 *)ptep);
}

static bool ept_entry_huge(void *ptep)
{
	return is_large_pte(*(u64 *)ptep);
}

static void ept_entry_mkhuge(void *ptep)
{
	*(u64 *)ptep |= PT_PAGE_SIZE_MASK;
}

static unsigned long ept_entry_to_phys(void *ptep)
{
	return *(u64 *)ptep & SPTE_BASE_ADDR_MASK;
}

static u64 ept_entry_to_prot(void *ptep)
{
	u64 prot = *(u64 *)ptep & ~(SPTE_BASE_ADDR_MASK);

	return prot & ~PT_PAGE_SIZE_MASK;
}

static int ept_entry_to_index(unsigned long vaddr, int level)
{
	return SPTE_INDEX(vaddr, level);
}

static bool ept_entry_is_leaf(void *ptep, int level)
{
	if (level == PG_LEVEL_4K ||
		!ept_entry_present(ptep) ||
		ept_entry_huge(ptep))
		return true;

	return false;

}

static int ept_level_entry_size(int level)
{
	return PAGE_SIZE / SPTE_ENT_PER_PAGE;
}

static int ept_level_to_entries(int level)
{
	return SPTE_ENT_PER_PAGE;
}

static u64 ept_level_page_mask(int level)
{
	return (~((1UL << SPTE_LEVEL_SHIFT(level)) - 1));
}

static unsigned long ept_level_to_size(int level)
{
	return KVM_HPAGE_SIZE(level);
}

static void ept_set_entry(void *sptep, u64 spte)
{
	WRITE_ONCE(*(u64 *)sptep, spte);
}

const struct pkvm_pgtable_ops ept_ops = {
	.pgt_entry_present = ept_entry_present,
	.pgt_entry_mapped = ept_entry_mapped,
	.pgt_entry_huge = ept_entry_huge,
	.pgt_entry_mkhuge = ept_entry_mkhuge,
	.pgt_entry_to_phys = ept_entry_to_phys,
	.pgt_entry_to_prot = ept_entry_to_prot,
	.pgt_entry_to_index = ept_entry_to_index,
	.pgt_level_page_mask = ept_level_page_mask,
	.pgt_entry_is_leaf = ept_entry_is_leaf,
	.pgt_level_entry_size = ept_level_entry_size,
	.pgt_level_to_entries = ept_level_to_entries,
	.pgt_level_to_size = ept_level_to_size,
	.pgt_set_entry = ept_set_entry,
};

bool is_pgt_ops_ept(struct pkvm_pgtable *pgt)
{
	return pgt && (pgt->pgt_ops == &ept_ops);
}

int pkvm_host_ept_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot)
{
	return pkvm_pgtable_map(&host_ept, vaddr_start, phys_start, size,
				pgsz_mask, prot, NULL, NULL);
}

int pkvm_host_ept_unmap(unsigned long vaddr_start, unsigned long phys_start,
			unsigned long size)
{
	return pkvm_pgtable_unmap_safe(&host_ept, vaddr_start, phys_start, size, NULL);
}

void pkvm_host_ept_lookup(unsigned long vaddr, unsigned long *pphys,
			  u64 *pprot, int *plevel)
{
	pkvm_pgtable_lookup(&host_ept, vaddr, pphys, pprot, plevel);
}

void host_ept_lock(void)
{
	pkvm_spin_lock(&_host_ept_lock);
}

void host_ept_unlock(void)
{
	pkvm_spin_unlock(&_host_ept_lock);
}

void pkvm_flush_host_ept(void)
{
	u64 eptp = pkvm_construct_eptp(host_ept.root_pa, host_ept.level);

	flush_ept(eptp);
}

int pkvm_host_ept_init(struct pkvm_pgtable_cap *cap,
		void *ept_pool_base, unsigned long ept_pool_pages)
{
	unsigned long pfn = __pkvm_pa(ept_pool_base) >> PAGE_SHIFT;
	int ret;

	ret = hyp_pool_init(&host_ept_pool, pfn, ept_pool_pages, 0);
	if (ret)
		return ret;

	pkvm_hyp->host_vm.ept = &host_ept;
	ret = pkvm_pgtable_init(&host_ept, &host_ept_mm_ops, &ept_ops, cap, true);
	if (ret)
		return ret;

	/*
	 * Prepare an instance for host EPT without doing TLB flushing.
	 * This is used for some fastpath code which wants to avoid
	 * doing TLB flushing for each host EPT modifications. It doesn't
	 * mean TLB flushing is not needed. The user still needs to do
	 * TLB flushing explicitly after finishing all the host EPT
	 * modifications.
	 */
	host_ept_notlbflush = host_ept;
	host_ept_notlbflush.mm_ops = &host_ept_mm_ops_no_tlbflush;
	pkvm_hyp->host_vm.ept_notlbflush = &host_ept_notlbflush;

	return 0;
}

static bool is_pvmfw_memory(unsigned long pa)
{
	return pvmfw_present && pa >= pvmfw_base && pa < pvmfw_base + pvmfw_size;
}

int handle_host_ept_violation(struct kvm_vcpu *vcpu)
{
	unsigned long hpa, gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);
	struct mem_range range, cur;
	bool is_memory = find_mem_range(gpa, &range) || is_pvmfw_memory(gpa);
	u64 prot = pkvm_mkstate(HOST_EPT_DEF_MMIO_PROT, PKVM_PAGE_OWNED);
	int level;
	int ret;

	if (is_memory) {
		pkvm_err("%s: not handle for memory address 0x%lx\n", __func__, gpa);
		return -EPERM;
	}

	pkvm_spin_lock(&_host_ept_lock);

	pkvm_pgtable_lookup(&host_ept, gpa, &hpa, NULL, &level);
	if (hpa != INVALID_ADDR) {
		ret = -EAGAIN;
		goto out;
	}

	do {
		unsigned long size = ept_level_to_size(level);

		cur.start = ALIGN_DOWN(gpa, size);
		cur.end = cur.start + size - 1;
		/*
		 * TODO:
		 * check if this MMIO belongs to a secure VM pass-through device.
		 */
		if ((1 << level & host_ept.allowed_pgsz) &&
				mem_range_included(&cur, &range) &&
				!is_mem_range_overlap_iommu(cur.start, cur.end))
			break;
		level--;
	} while (level != PG_LEVEL_NONE);

	if (level == PG_LEVEL_NONE) {
		pkvm_err("pkvm: No valid range: gpa 0x%lx, cur 0x%lx ~ 0x%lx size 0x%lx level %d\n",
			 gpa, cur.start, cur.end, cur.end - cur.start + 1, level);
		ret = -EPERM;
		goto out;
	}

	pkvm_dbg("pkvm: %s: cur MMIO range 0x%lx ~ 0x%lx size 0x%lx level %d\n",
		__func__, cur.start, cur.end, cur.end - cur.start + 1, level);

	ret = pkvm_host_ept_map(cur.start, cur.start, cur.end - cur.start + 1,
			   1 << level, prot);
	if (ret == -ENOMEM) {
		/* TODO: reclaim MMIO range pages first and try do map again */
		pkvm_dbg("%s: no memory to set host ept for addr 0x%lx\n",
			 __func__, gpa);
	}
out:
	pkvm_spin_unlock(&_host_ept_lock);

	return ret;
}

int pkvm_shadow_ept_pool_init(void *ept_pool_base, unsigned long ept_pool_pages)
{
	unsigned long pfn = __pkvm_pa(ept_pool_base) >> PAGE_SHIFT;

	return hyp_pool_init(&shadow_pgt_pool, pfn, ept_pool_pages, 0);
}

static void *shadow_pgt_zalloc_page(struct pkvm_memcache *mc)
{
	return hyp_alloc_pages(&shadow_pgt_pool, 0);
}

static void shadow_pgt_get_page(void *vaddr)
{
	hyp_get_page(&shadow_pgt_pool, vaddr);
}

static void shadow_pgt_put_page(void *vaddr)
{
	hyp_put_page(&shadow_pgt_pool, vaddr);
}

/*
 * mm_ops for shadow second-level IOMMU page tables. These tables
 * are similar to shadow EPT tables, as they also have the EPT
 * format and their memory is reserved together with shadow EPT
 * pages. The difference is that this mm_ops doesn't have the
 * flush_tlb callback.
 *
 * Precisely, shadow_sl_iommu_pgt_mm_ops is used for two kinds of
 * 2nd level iommu page tables:
 *
 * - pgstate_pgt which is reused as IOMMU page table for protected
 *   VM with passthrough devices. In this case the memory is pinned,
 *   and the mapping is not allowed to be removed from pgstate_pgt,
 *   so the flush_tlb callback is not needed.
 *
 * - Host shadow IOMMU page tables used for the host's devices when
 *   legacy IOMMU is used. They do not need the flush_tlb callback
 *   either, since IOTLB flush after unmapping pages from these
 *   tables is performed in other ways: either as a part of vIOMMU
 *   IOTLB flush emulation when initiated by the host, or together
 *   with host EPT TLB flush when ensuring pKVM memory protection.
 *
 * TODO: refactor the code: move all the management of both types
 * of 2nd level iommu page tables to iommu_spgt.c to some common API.
 * That means also refactoring of pkvm_ptdev structure.
 */
static const struct pkvm_mm_ops shadow_sl_iommu_pgt_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = shadow_pgt_zalloc_page,
	.get_page = shadow_pgt_get_page,
	.put_page = shadow_pgt_put_page,
	.page_count = hyp_page_count,
};

/*
 * Flushing cache is needed when modifying IOMMU page table entries
 * if the IOMMU is not coherent. This ops has flush_cache callback
 * so it can be used for a pgtable which is used as IOMMU page table
 * with noncoherent IOMMU.
 */
static const struct pkvm_mm_ops shadow_sl_iommu_pgt_mm_ops_noncoherency = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = shadow_pgt_zalloc_page,
	.get_page = shadow_pgt_get_page,
	.put_page = shadow_pgt_put_page,
	.page_count = hyp_page_count,
	.flush_cache = pkvm_clflush_cache_range,
};

static int pkvm_pgstate_pgt_free_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
				      void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	unsigned long phys = pgt->pgt_ops->pgt_entry_to_phys(ptep);
	unsigned long size = pgt->pgt_ops->pgt_level_to_size(level);
	struct pkvm_shadow_vm *vm = pgstate_pgt_to_shadow_vm(pgt);
	int ret;

	if (!pgt->pgt_ops->pgt_entry_present(ptep))
		return 0;

	/*
	 * For normal VM, call __pkvm_host_unshare_guest() to unshare all previous
	 * shared pages, the page table entry with present bits indicate the page
	 * was shared before.
	 *
	 * For protected VM, call __pkvm_host_undonate_guest() to undonate all
	 * previous donated pages, the donated pages are indicated by their page
	 * table entry which is present.
	 *
	 * And the pgtable_free_cb in this current page walker is still walking
	 * the page state table so cannot allow the  __pkvm_host_unshare_guest()
	 * or __pkvm_host_undonate_guest() release page state table pages. So
	 * we shall get_page before these APIs called, then put_page to allow
	 * pgtable_free_cb free table pages with correct refcount.
	 *
	 */
	if (!shadow_vm_is_protected(vm)) {
		pgt->mm_ops->get_page(ptep);
		ret = __pkvm_host_unshare_guest(phys, pgt, vaddr, size);
		pgt->mm_ops->put_page(ptep);
		flush_data->flushtlb |= true;
	} else {
		struct mem_range range;
		/*
		 * before returning to host, the memory page previously owned by
		 * protected VM shall be memset to 0 to avoid secret leakage.
		 */
		if (find_mem_range(phys, &range))
			memset(pgt->mm_ops->phys_to_virt(phys), 0, min(size, range.end - phys));
		pgt->mm_ops->get_page(ptep);
		ret = __pkvm_host_undonate_guest(phys, pgt, vaddr, size);
		pgt->mm_ops->put_page(ptep);
		flush_data->flushtlb |= true;
	}

	if (ret)
		pkvm_err("%s failed: ret %d vm_type %d phys 0x%lx GPA 0x%lx size 0x%lx\n",
			 __func__, ret, vm->vm_type, phys, vaddr, size);
	return ret;
}

void pkvm_pgstate_pgt_deinit(struct pkvm_shadow_vm *vm)
{
	pkvm_spin_lock(&vm->lock);

	pkvm_pgtable_destroy(&vm->pgstate_pgt, pkvm_pgstate_pgt_free_leaf);

	pkvm_spin_unlock(&vm->lock);
}

int pkvm_pgstate_pgt_init(struct pkvm_shadow_vm *vm)
{
	struct pkvm_pgtable *pgt = &vm->pgstate_pgt;
	struct pkvm_pgtable_cap cap = {
		.level = pkvm_hyp->ept_iommu_pgt_level,
		.allowed_pgsz = pkvm_hyp->ept_iommu_pgsz_mask,
		.table_prot = VMX_EPT_RWX_MASK,
	};

	return pkvm_pgtable_init(pgt, &shadow_sl_iommu_pgt_mm_ops, &ept_ops, &cap, true);
}

const struct pkvm_mm_ops *pkvm_shadow_sl_iommu_pgt_get_mm_ops(bool coherent)
{
	return coherent ? &shadow_sl_iommu_pgt_mm_ops
			: &shadow_sl_iommu_pgt_mm_ops_noncoherency;
}

void pkvm_shadow_sl_iommu_pgt_update_coherency(struct pkvm_pgtable *pgt, bool coherent)
{
	if (coherent)
		pkvm_pgtable_set_mm_ops(pgt, &shadow_sl_iommu_pgt_mm_ops);
	else
		pkvm_pgtable_set_mm_ops(pgt, &shadow_sl_iommu_pgt_mm_ops_noncoherency);
}
