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

static struct hyp_pool host_ept_pool;
static struct pkvm_pgtable host_ept;
static struct pkvm_pgtable host_ept_notlbflush;
static pkvm_spinlock_t _host_ept_lock = __PKVM_SPINLOCK_UNLOCKED;

static struct hyp_pool shadow_pgt_pool;
static struct rsvd_bits_validate ept_zero_check;

static void flush_tlb_noop(struct pkvm_pgtable *pgt,
			   unsigned long addr, unsigned long size)
{
}

static inline void pkvm_init_ept_page(void *page)
{
	/*
	 * Normal VM: Never clear the "suppress #VE" bit, so #VE will never
	 * be triggered.
	 *
	 * Protected VM: pkvm sets EPT_VIOLATION_VE for Protected VM, "suppress
	 * #VE" bit must be set to get EPT violation, thus pkvm can build the
	 * EPT mapping for memory region, and clear "suppress #VE" for mmio
	 * region, thus mmio can trigger #VE.
	 *
	 * For simplicity, unconditionally initialize SEPT to set "suppress
	 * #VE".
	 */
	memset64((u64 *)page, EPT_PROT_DEF, 512);
}

static void *ept_zalloc_page(struct hyp_pool *pool)
{
	void *page;

	page = hyp_alloc_pages(pool, 0);
	if (page)
		pkvm_init_ept_page(page);

	return page;
}

static void *host_ept_zalloc_page(void)
{
	/*
	 * Also initiailize the host ept with SUPPRESS_VE bit set although this
	 * bit is ignored in host ept. Because host_ept and shadow_ept share the
	 * same ept_ops, this will make the ept_entry_mapped work for both
	 * host_ept and shadow_ept.
	 */
	return ept_zalloc_page(&host_ept_pool);
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

	/*
	 * Also needs to flush the IOTLB as host EPT is used
	 * as second-stage page table for some devices.
	 */
	pkvm_iommu_flush_iotlb(pgt, vaddr, size);
}

static void host_ept_flush_cache(void *vaddr, unsigned int size)
{
	if (!pkvm_hyp->iommu_coherent)
		pkvm_clflush_cache_range(vaddr, size);
}

struct pkvm_mm_ops host_ept_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = host_ept_zalloc_page,
	.get_page = host_ept_get_page,
	.put_page = host_ept_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = host_ept_flush_tlb,
	.flush_cache = host_ept_flush_cache,
};

static struct pkvm_mm_ops host_ept_mm_ops_no_tlbflush = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = host_ept_zalloc_page,
	.get_page = host_ept_get_page,
	.put_page = host_ept_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = flush_tlb_noop,
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
	 *
	 * Since we initialize every pte with SUPPRESS_VE bit set, which means
	 * if a pte does not equal to the default value, it has been mapped.
	 */
	return !(*(u64 *)ptep == EPT_PROT_DEF);
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

struct pkvm_pgtable_ops ept_ops = {
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
	.default_prot = EPT_PROT_DEF,
};

bool is_pgt_ops_ept(struct pkvm_pgtable *pgt)
{
	return pgt && (pgt->pgt_ops == &ept_ops);
}

int pkvm_host_ept_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot)
{
	return pkvm_pgtable_map(&host_ept, vaddr_start, phys_start, size,
				pgsz_mask, prot, NULL);
}

int pkvm_host_ept_unmap(unsigned long vaddr_start, unsigned long phys_start,
			unsigned long size)
{
	return pkvm_pgtable_unmap_safe(&host_ept, vaddr_start, phys_start, size, NULL);
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

static void reset_rsvds_bits_mask_ept(struct rsvd_bits_validate *rsvd_check,
				      u64 pa_bits_rsvd, bool execonly,
				      int huge_page_level)
{
	u64 high_bits_rsvd = pa_bits_rsvd & rsvd_bits(0, 51);
	u64 large_1g_rsvd = 0, large_2m_rsvd = 0;
	u64 bad_mt_xwr;

	if (huge_page_level < PG_LEVEL_1G)
		large_1g_rsvd = rsvd_bits(7, 7);
	if (huge_page_level < PG_LEVEL_2M)
		large_2m_rsvd = rsvd_bits(7, 7);

	rsvd_check->rsvd_bits_mask[0][4] = high_bits_rsvd | rsvd_bits(3, 7);
	rsvd_check->rsvd_bits_mask[0][3] = high_bits_rsvd | rsvd_bits(3, 7);
	rsvd_check->rsvd_bits_mask[0][2] = high_bits_rsvd | rsvd_bits(3, 6) | large_1g_rsvd;
	rsvd_check->rsvd_bits_mask[0][1] = high_bits_rsvd | rsvd_bits(3, 6) | large_2m_rsvd;
	rsvd_check->rsvd_bits_mask[0][0] = high_bits_rsvd;

	/* large page */
	rsvd_check->rsvd_bits_mask[1][4] = rsvd_check->rsvd_bits_mask[0][4];
	rsvd_check->rsvd_bits_mask[1][3] = rsvd_check->rsvd_bits_mask[0][3];
	rsvd_check->rsvd_bits_mask[1][2] = high_bits_rsvd | rsvd_bits(12, 29) | large_1g_rsvd;
	rsvd_check->rsvd_bits_mask[1][1] = high_bits_rsvd | rsvd_bits(12, 20) | large_2m_rsvd;
	rsvd_check->rsvd_bits_mask[1][0] = rsvd_check->rsvd_bits_mask[0][0];

	bad_mt_xwr = 0xFFull << (2 * 8);	/* bits 3..5 must not be 2 */
	bad_mt_xwr |= 0xFFull << (3 * 8);	/* bits 3..5 must not be 3 */
	bad_mt_xwr |= 0xFFull << (7 * 8);	/* bits 3..5 must not be 7 */
	bad_mt_xwr |= REPEAT_BYTE(1ull << 2);	/* bits 0..2 must not be 010 */
	bad_mt_xwr |= REPEAT_BYTE(1ull << 6);	/* bits 0..2 must not be 110 */
	if (!execonly) {
		/* bits 0..2 must not be 100 unless VMX capabilities allow it */
		bad_mt_xwr |= REPEAT_BYTE(1ull << 4);
	}
	rsvd_check->bad_mt_xwr = bad_mt_xwr;
}

int pkvm_host_ept_init(struct pkvm_pgtable_cap *cap,
		void *ept_pool_base, unsigned long ept_pool_pages)
{
	unsigned long pfn = __pkvm_pa(ept_pool_base) >> PAGE_SHIFT;
	int ret;
	u8 pa_bits;

	ret = hyp_pool_init(&host_ept_pool, pfn, ept_pool_pages, 0);
	if (ret)
		return ret;

	pa_bits = get_max_physaddr_bits();
	if (!pa_bits)
		return -EINVAL;
	reset_rsvds_bits_mask_ept(&ept_zero_check, rsvd_bits(pa_bits, 63),
				  vmx_has_ept_execute_only(),
				  fls(cap->allowed_pgsz) - 1);

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

int handle_host_ept_violation(unsigned long gpa)
{
	unsigned long hpa;
	struct mem_range range, cur;
	bool is_memory = find_mem_range(gpa, &range);
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

static void *shadow_pgt_zalloc_page(void)
{
	return ept_zalloc_page(&shadow_pgt_pool);
}

static void shadow_pgt_get_page(void *vaddr)
{
	hyp_get_page(&shadow_pgt_pool, vaddr);
}

static void shadow_pgt_put_page(void *vaddr)
{
	hyp_put_page(&shadow_pgt_pool, vaddr);
}

static void shadow_ept_flush_tlb(struct pkvm_pgtable *pgt,
				 unsigned long addr,
				 unsigned long size)
{
	struct pkvm_shadow_vm *shadow_vm = sept_to_shadow_vm(pgt);
	struct shadow_vcpu_state *shadow_vcpu;
	struct kvm_vcpu *vcpu;
	s64 shadow_vcpu_handle;
	int i, shadow_vm_handle = shadow_vm->shadow_vm_handle;

	for (i = 0; i < shadow_vm->created_vcpus; i++) {
		shadow_vcpu_handle = to_shadow_vcpu_handle(shadow_vm_handle, i);
		shadow_vcpu = get_shadow_vcpu(shadow_vcpu_handle);
		/*
		 * For a shadow_vcpu which is already teardown, no need to kick
		 * it as its shadow EPT tlb entries are already flushed when
		 * this shadow vcpu is doing vmclear before teardown.
		 */
		if (!shadow_vcpu)
			continue;

		/*
		 * If this shadow_vcpu is not loaded then there is vcpu
		 * pointer for it, so can skip this remote tlb flushing.
		 */
		vcpu = READ_ONCE(shadow_vcpu->vcpu);
		if (!vcpu)
			goto next;

		kvm_make_request(PKVM_REQ_TLB_FLUSH_SHADOW_EPT, vcpu);
		pkvm_kick_vcpu(vcpu);
next:
		put_shadow_vcpu(shadow_vcpu_handle);
	}
}

static struct pkvm_mm_ops shadow_ept_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = shadow_pgt_zalloc_page,
	.get_page = shadow_pgt_get_page,
	.put_page = shadow_pgt_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = shadow_ept_flush_tlb,
};

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
static struct pkvm_mm_ops shadow_sl_iommu_pgt_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = shadow_pgt_zalloc_page,
	.get_page = shadow_pgt_get_page,
	.put_page = shadow_pgt_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = flush_tlb_noop,
};

/*
 * Flushing cache is needed when modifying IOMMU page table entries
 * if the IOMMU is not coherent. This ops has flush_cache callback
 * so it can be used for a pgtable which is used as IOMMU page table
 * with noncoherent IOMMU.
 */
static struct pkvm_mm_ops shadow_sl_iommu_pgt_mm_ops_noncoherency = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = shadow_pgt_zalloc_page,
	.get_page = shadow_pgt_get_page,
	.put_page = shadow_pgt_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = flush_tlb_noop,
	.flush_cache = pkvm_clflush_cache_range,
};

static int pkvm_pgstate_pgt_map_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
				     void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	struct pkvm_pgtable_map_data *data = arg;
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	unsigned long level_size = pgt_ops->pgt_level_to_size(level);
	unsigned long map_phys = data->phys & PAGE_MASK;
	struct pkvm_shadow_vm *vm = pgstate_pgt_to_shadow_vm(pgt);
	int ret;

	/*
	 * It is possible that another CPU just created same mapping when
	 * multiple EPT violations happen on different CPUs.
	 */
	if (pgt_ops->pgt_entry_present(ptep)) {
		unsigned long phys = pgt_ops->pgt_entry_to_phys(ptep);

		/*
		 * Check if the existing mapping is the same as the wanted one.
		 * If not the same, report an error so that the map_leaf caller
		 * will not map the different addresses in its shadow EPT.
		 */
		if (phys != map_phys) {
			pkvm_err("%s: gpa 0x%lx @level%d old_phys 0x%lx != new_phys 0x%lx\n",
				 __func__, vaddr, level, phys, map_phys);
			return -EPERM;
		}

		/*
		 * The pgstate_pgt now is EPT format with fixed property bits. No
		 * need to check and update property bits for pgstate_pgt.
		 */
		goto out;
	}

	switch (vm->vm_type) {
	case KVM_X86_DEFAULT_VM:
		ret = __pkvm_host_share_guest(map_phys, pgt, vaddr, level_size, data->prot);
		break;
	case KVM_X86_PROTECTED_VM:
		if (vm->need_prepopulation)
			/*
			 * As pgstate pgt is the source of the shadow EPT, only after pgstate
			 * pgt is set up, shadow EPT can be set up. So protected VM will not be
			 * able to use the memory donated in pgstate pgt before its shadow EPT
			 * is setting up. So it is safe to use the fastpath to donate all the
			 * pages to improve the pre-population performance. TLB flushing
			 * can be done in the caller after the pre-population is done but before
			 * setting up its shadow EPT.
			 */
			ret = __pkvm_host_donate_guest_fastpath(map_phys, pgt, vaddr,
								level_size, data->prot);
		else
			ret = __pkvm_host_donate_guest(map_phys, pgt, vaddr,
						       level_size, data->prot);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret) {
		pkvm_err("%s failed: ret %d vm_type %d L2 GPA 0x%lx level %d HPA 0x%lx prot 0x%llx\n",
			 __func__, ret, vm->vm_type, vaddr, level, map_phys, data->prot);
		return ret;
	}

out:
	/* Increase the physical address for the next mapping */
	data->phys += level_size;

	return 0;
}

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
	switch(vm->vm_type) {
	case KVM_X86_DEFAULT_VM:
		pgt->mm_ops->get_page(ptep);
		ret = __pkvm_host_unshare_guest(phys, pgt, vaddr, size);
		pgt->mm_ops->put_page(ptep);
		flush_data->flushtlb |= true;
		break;
	case KVM_X86_PROTECTED_VM: {
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
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		pkvm_err("%s failed: ret %d vm_type %d phys 0x%lx GPA 0x%lx size 0x%lx\n",
			 __func__, ret, vm->vm_type, phys, vaddr, size);
	return ret;
}

static void __invalidate_shadow_ept_with_range(struct shadow_ept_desc *desc,
					       unsigned long vaddr, unsigned long size)
{
	struct pkvm_shadow_vm *vm = sept_desc_to_shadow_vm(desc);
	struct pkvm_pgtable *sept = &desc->sept;

	if (!size)
		return;

	pkvm_spin_lock(&vm->lock);

	if (!is_valid_eptp(desc->shadow_eptp))
		goto out;

	pkvm_pgtable_unmap_nosplit(sept, vaddr, size, NULL);

	/*
	 * As for normal VM, its memory might need to be swapped out
	 * or other kinds of management from primary VM thus should
	 * unmap from pgstate pgt as well.
	 *
	 * As for protected VM, its memory is pinned thus no need to
	 * unmap from pgstate pgt.
	 */
	if (vm->vm_type == KVM_X86_DEFAULT_VM)
		pkvm_pgtable_unmap_nosplit(&vm->pgstate_pgt, vaddr, size,
					   pkvm_pgstate_pgt_free_leaf);
out:
	pkvm_spin_unlock(&vm->lock);
}

void pkvm_invalidate_shadow_ept(struct shadow_ept_desc *desc)
{
	struct pkvm_pgtable *sept = &desc->sept;
	unsigned long size = sept->pgt_ops->pgt_level_to_size(sept->level + 1);

	__invalidate_shadow_ept_with_range(desc, 0, size);
}

void pkvm_invalidate_shadow_ept_with_range(struct shadow_ept_desc *desc,
					   unsigned long vaddr, unsigned long size)
{
	__invalidate_shadow_ept_with_range(desc, vaddr, size);
}

void pkvm_shadow_ept_deinit(struct shadow_ept_desc *desc)
{
	struct pkvm_shadow_vm *vm = sept_desc_to_shadow_vm(desc);

	pkvm_spin_lock(&vm->lock);

	if (desc->shadow_eptp)
		pkvm_pgtable_destroy(&desc->sept, NULL);

	memset(desc, 0, sizeof(struct shadow_ept_desc));

	pkvm_spin_unlock(&vm->lock);
}

int pkvm_shadow_ept_init(struct shadow_ept_desc *desc)
{
	struct pkvm_pgtable_cap cap = {
		.level = pkvm_hyp->ept_cap.level,
		.allowed_pgsz = pkvm_hyp->ept_cap.allowed_pgsz,
		.table_prot = pkvm_hyp->ept_cap.table_prot,
	};
	int ret;

	memset(desc, 0, sizeof(struct shadow_ept_desc));

	ret = pkvm_pgtable_init(&desc->sept, &shadow_ept_mm_ops, &ept_ops, &cap, true);
	if (ret)
		return ret;

	desc->shadow_eptp = pkvm_construct_eptp(desc->sept.root_pa, cap.level);
	flush_ept(desc->shadow_eptp);

	return 0;
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

struct pkvm_mm_ops *pkvm_shadow_sl_iommu_pgt_get_mm_ops(bool coherent)
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

/*
 * virtual_ept_mm_ops is used as the ops for the ept constructed by
 * KVM high in host.
 * The physical address in this ept is the host VM GPA, which is
 * the same with HPA.
 */
struct pkvm_mm_ops virtual_ept_mm_ops = {
	.phys_to_virt = host_gpa2hva,
};

void pkvm_guest_ept_deinit(struct shadow_vcpu_state *shadow_vcpu)
{
	struct pkvm_pgtable *vept = &shadow_vcpu->vept;

	memset(vept, 0, sizeof(struct pkvm_pgtable));
}

void pkvm_guest_ept_init(struct shadow_vcpu_state *shadow_vcpu, u64 guest_eptp)
{
	/*
	 * TODO: we just assume guest will use page level the HW supported,
	 * it actually need align with KVM high
	 */
	struct pkvm_pgtable_cap cap = {
		.level = pkvm_hyp->ept_cap.level,
		.allowed_pgsz = pkvm_hyp->ept_cap.allowed_pgsz,
		.table_prot = pkvm_hyp->ept_cap.table_prot,
	};

	pkvm_pgtable_init(&shadow_vcpu->vept, &virtual_ept_mm_ops, &ept_ops, &cap, false);
	shadow_vcpu->vept.root_pa = host_gpa2hpa(guest_eptp & SPTE_BASE_ADDR_MASK);
}

static bool is_access_violation(u64 ept_entry, u64 exit_qual)
{
	bool access_violation = false;

	if (/* Caused by data read */
	    (((exit_qual & 0x1UL) != 0UL) && ((ept_entry & VMX_EPT_READABLE_MASK) == 0)) ||
	    /* Caused by data write */
	    (((exit_qual & 0x2UL) != 0UL) && ((ept_entry & VMX_EPT_WRITABLE_MASK) == 0)) ||
	    /* Caused by instruction fetch */
	    (((exit_qual & 0x4UL) != 0UL) && ((ept_entry & VMX_EPT_EXECUTABLE_MASK) == 0))) {
		access_violation = true;
	}

	return access_violation;
}

static int populate_pgstate_pgt(struct pkvm_pgtable *pgt)
{
	struct pkvm_shadow_vm *vm = pgstate_pgt_to_shadow_vm(pgt);
	struct list_head *ptdev_head = &vm->ptdev_head;
	struct pkvm_ptdev *ptdev, *tmp;
	u64 *prot_override;
	bool populated;
	u64 prot;
	int ret;

	list_for_each_entry(ptdev, ptdev_head, vm_node) {
		/* No need to populate if vpgt.root_pa doesn't exist */
		if (!ptdev->vpgt.root_pa)
			continue;

		populated = false;
		list_for_each_entry(tmp, ptdev_head, vm_node) {
			if (tmp == ptdev)
				break;
			if (tmp->vpgt.root_pa == ptdev->vpgt.root_pa) {
				populated = true;
				break;
			}
		}

		if (populated)
			continue;

		if (ptdev->vpgt.pgt_ops != pgt->pgt_ops) {
			/* Populate with EPT format */
			if (is_pgt_ops_ept(pgt)) {
				prot = VMX_EPT_RWX_MASK;
			} else {
				pkvm_err("pkvm: not supported populating\n");
				return -EOPNOTSUPP;
			}
			prot_override = &prot;
		} else {
			prot_override = NULL;
		}

		ret = pkvm_pgtable_sync_map(&ptdev->vpgt, pgt, prot_override,
					    pkvm_pgstate_pgt_map_leaf);
		if (ret)
			return ret;
	}

	return 0;
}

static bool allow_shadow_ept_mapping(struct pkvm_shadow_vm *vm,
				     u64 gpa, unsigned long hpa,
				     unsigned long size)
{
	struct pkvm_pgtable *pgstate_pgt = &vm->pgstate_pgt;
	unsigned long mapped_hpa;
	int level;

	/*
	 * VM will be marked as need_prepopulation when a passthrough device is
	 * attached. With this flag being set, VM's pgstate_pgt will be pre-populated
	 * before handling EPT violation. After the population is done, this flag
	 * can be cleared.
	 */
	if (vm->need_prepopulation) {
		unsigned long size;

		if (populate_pgstate_pgt(pgstate_pgt))
			return false;
		/*
		 * Explicitly flush TLB of the host EPT after populating the page
		 * state pgt.
		 *
		 * During the population, some pages are donated from primary VM to
		 * this VM with the fastpath interface to avoid doing TLB flushing
		 * during each iteration of the page donation so that to have a fast
		 * population performance. So still need to do TLB flushing in the
		 * end after finishing all the donations.
		 */
		size = host_ept.pgt_ops->pgt_level_to_size(host_ept.level + 1);
		host_ept_flush_tlb(&host_ept, 0, size);
		vm->need_prepopulation = false;
	}

	/*
	 * Lookup the page state pgt to check if the mapping is already created
	 * or not.
	 */
	pkvm_pgtable_lookup(pgstate_pgt, gpa, &mapped_hpa, NULL, &level);

	if ((pgstate_pgt->pgt_ops->pgt_level_to_size(level) < size) ||
	    mapped_hpa == INVALID_ADDR) {
		u64 prot;
		/*
		 * Page state pgt doesn't have mapping yet, or it has mapping
		 * but with a smaller size, so try to map with the desired size
		 * in page state pgt first. Although page state pgt may already
		 * have all the desired mappings with smaller size, map_leaf
		 * can help to check if the mapped phys matches with the desired
		 * hpa to guarantee shadow EPT maps GPA to the right HPA.
		 */
		if (is_pgt_ops_ept(pgstate_pgt)) {
			prot = VMX_EPT_RWX_MASK;
		} else {
			pkvm_err("%s: pgstate_pgt format not supported\n", __func__);
			return false;
		}

		if (pkvm_pgtable_map(pgstate_pgt, gpa, hpa, size,
				     0, prot, pkvm_pgstate_pgt_map_leaf)) {
			pkvm_err("%s: pgstate_pgt map gpa 0x%llx hpa 0x%lx size 0x%lx failed\n",
				 __func__, gpa, hpa, size);
			return false;
		}
	} else if (mapped_hpa != hpa) {
		/*
		 * Page state pgt has mapping already, so check if the mapped
		 * phys matches with the hpa, and report an error if doesn't
		 * match.
		 */
		pkvm_err("pgstate_pgt not match: mapped_hpa 0x%lx != 0x%lx for gpa 0x%llx\n",
			 mapped_hpa, hpa, gpa);
		return false;
	}

	return true;
}

enum sept_handle_ret
pkvm_handle_shadow_ept_violation(struct shadow_vcpu_state *shadow_vcpu, u64 l2_gpa, u64 exit_quali)
{
	struct pkvm_shadow_vm *vm = shadow_vcpu->vm;
	struct shadow_ept_desc *desc = &vm->sept_desc;
	struct pkvm_pgtable *sept = &desc->sept;
	struct pkvm_pgtable_ops *pgt_ops = sept->pgt_ops;
	struct pkvm_pgtable *vept = &shadow_vcpu->vept;
	enum sept_handle_ret ret = PKVM_NOT_HANDLED;
	unsigned long phys;
	int level;
	u64 gprot, rsvd_chk_gprot;

	pkvm_spin_lock(&vm->lock);

	pkvm_pgtable_lookup(vept, l2_gpa, &phys, &gprot, &level);
	if (phys == INVALID_ADDR)
		/* Geust EPT not valid, back to kvm-high */
		goto out;

	if (is_access_violation(gprot, exit_quali))
		/* Guest EPT error, refuse to handle in shadow ept */
		goto out;

	rsvd_chk_gprot = gprot;
	/* is_rsvd_spte() need based on PAGE_SIZE bit */
	if (level != PG_LEVEL_4K)
		pgt_ops->pgt_entry_mkhuge(&rsvd_chk_gprot);

	if (is_rsvd_spte(&ept_zero_check, rsvd_chk_gprot, level)) {
		ret = PKVM_INJECT_EPT_MISC;
	} else {
		unsigned long level_size = pgt_ops->pgt_level_to_size(level);
		unsigned long gpa = ALIGN_DOWN(l2_gpa, level_size);
		unsigned long hpa = ALIGN_DOWN(host_gpa2hpa(phys), level_size);
		/*
		 * Still set SUPPRESS_VE bit here as some mapping may still
		 * cause EPT_VIOLATION and we want these EPT_VIOLATION to cause
		 * vmexit.
		 */
		u64 prot = (gprot & EPT_PROT_MASK) | EPT_PROT_DEF;

		if (allow_shadow_ept_mapping(vm, gpa, hpa, level_size) &&
		    !pkvm_pgtable_map(sept, gpa, hpa, level_size, 0, prot, NULL))
			ret = PKVM_HANDLED;
	}
out:
	pkvm_spin_unlock(&vm->lock);
	return ret;
}

void pkvm_flush_shadow_ept(struct shadow_ept_desc *desc)
{
	if (!is_valid_eptp(desc->shadow_eptp))
		return;

	flush_ept(desc->shadow_eptp);
}

void pkvm_shadow_clear_suppress_ve(struct kvm_vcpu *vcpu, unsigned long gfn)
{
	unsigned long gpa = gfn * PAGE_SIZE;
	struct pkvm_host_vcpu *pkvm_hvcpu = to_pkvm_hvcpu(vcpu);
	struct shadow_vcpu_state *shadow_vcpu = pkvm_hvcpu->current_shadow_vcpu;
	struct pkvm_shadow_vm *vm = shadow_vcpu->vm;
	struct shadow_ept_desc *desc = &vm->sept_desc;
	struct pkvm_pgtable *sept = &desc->sept;

	if (!shadow_vcpu_is_protected(shadow_vcpu))
		return;

	/*
	 * Set the mmio_pte with prot 0, which means it is invalid and with
	 * "Suppress #VE" bit cleared. Accessing this pte will trigger #VE.
	 */
	pkvm_pgtable_annotate(sept, gpa, PAGE_SIZE, SHADOW_EPT_MMIO_ENTRY);
}
