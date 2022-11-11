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

static struct hyp_pool host_ept_pool;
static struct pkvm_pgtable host_ept;
static pkvm_spinlock_t _host_ept_lock = __PKVM_SPINLOCK_UNLOCKED;

static struct hyp_pool shadow_ept_pool;
static struct rsvd_bits_validate ept_zero_check;

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
	asm volatile ("rep stosq\n\t"
		      :
		      : "a"(EPT_PROT_DEF), "c"(512), "D"(page)
		      : "memory"
	);
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

static void host_ept_flush_tlb(struct pkvm_pgtable *pgt)
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

static void ept_mk_nopresent(struct pkvm_pgtable *pgt, void *ptep)
{
	u64 val;

	val = READ_ONCE(*(u64 *)ptep) & ~VMX_EPT_RWX_MASK;
	pgt->pgt_ops->pgt_set_entry(ptep, val);
}

static void ept_remap_with_newprot(struct pkvm_pgtable *pgt, int level, void *ptep, u64 new_prot)
{
	u64 old_pte = READ_ONCE(*(u64 *)ptep);
	u64 new_pte;

	if ((old_pte & EPT_PROT_MASK) == new_prot)
		return;

	new_pte = (old_pte & ~EPT_PROT_MASK) | new_prot;
	if (level != PG_LEVEL_4K)
		pgt->pgt_ops->pgt_entry_mkhuge(&new_pte);
	pgt->pgt_ops->pgt_set_entry(ptep, new_pte);
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
	return pkvm_pgtable_init(&host_ept, &host_ept_mm_ops, &ept_ops, cap, true);
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

	return hyp_pool_init(&shadow_ept_pool, pfn, ept_pool_pages, 0);
}

static void *shadow_ept_zalloc_page(void)
{
	return ept_zalloc_page(&shadow_ept_pool);
}

static void shadow_ept_get_page(void *vaddr)
{
	hyp_get_page(&shadow_ept_pool, vaddr);
}

static void shadow_ept_put_page(void *vaddr)
{
	hyp_put_page(&shadow_ept_pool, vaddr);
}

static void shadow_ept_flush_tlb(struct pkvm_pgtable *pgt)
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
	.zalloc_page = shadow_ept_zalloc_page,
	.get_page = shadow_ept_get_page,
	.put_page = shadow_ept_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = shadow_ept_flush_tlb,
};

static int pkvm_shadow_ept_map_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
				    void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	struct pkvm_pgtable_map_data *data = arg;
	struct pkvm_pgtable_ops *pgt_ops = pgt->pgt_ops;
	unsigned long level_size = pgt_ops->pgt_level_to_size(level);
	unsigned long map_phys = data->phys & PAGE_MASK;
	struct pkvm_shadow_vm *vm = sept_to_shadow_vm(pgt);
	int ret;

	/*
	 * It is possible that another CPU just created same mapping when
	 * multiple EPT violations happen on different CPUs.
	 */
	if (pgt_ops->pgt_entry_present(ptep)) {
		/*
		 * Update the present entry with the newprot as a mismatching
		 * property bits can also cause EPT violation.
		 */
		ept_remap_with_newprot(pgt, level, ptep, data->prot);
		goto out;
	}

	switch (vm->vm_type) {
	case KVM_X86_DEFAULT_VM:
		ret = __pkvm_host_share_guest(map_phys, pgt, vaddr, level_size, data->prot);
		break;
	case KVM_X86_PROTECTED_VM:
		if (owned_this_page(ptep)) {
			unsigned long phys = pgt_ops->pgt_entry_to_phys(ptep);

			/*
			 * pkvm doesn't allow changing the final page mapping
			 * in shadow EPT if this page has been used by protected
			 * VM. This is due to security concern. So before reusing
			 * the mapping, do a sanity check and report an error if
			 * not the same.
			 */
			if (phys != map_phys) {
				pkvm_err("%s: gpa 0x%lx @level%d old_phys 0x%lx != new_phys 0x%lx\n",
						__func__, vaddr, level, phys, map_phys);
				ret = -EPERM;
			} else {
				/*
				 * Invept has invalid this entry for protected VM but keep
				 * the phys address remained. Re-use this phys address and
				 * its page state to create the mapping with new property
				 * bits.
				 */
				ept_remap_with_newprot(pgt, level, ptep, data->prot);
				ret = 0;
			}
		} else {
			ret = __pkvm_host_donate_guest(map_phys, pgt, vaddr, level_size, data->prot);
		}
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

static int pkvm_shadow_ept_free_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
				     void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	unsigned long phys = pgt->pgt_ops->pgt_entry_to_phys(ptep);
	unsigned long size = pgt->pgt_ops->pgt_level_to_size(level);
	struct pkvm_shadow_vm *vm = sept_to_shadow_vm(pgt);
	int ret = 0;

	/*
	 * For normal VM, call __pkvm_host_unshare_guest() to unshare all previous
	 * shared pages, the page table entry with present bits indicate the page
	 * was shared before.
	 *
	 * For protected VM, call __pkvm_host_undonate_guest() to undonate all
	 * previous donated pages, the donated pages are indicated by their page
	 * table entry whose page state show it owned this page - check by API
	 * owned_this_page(). The reason to check page state is because for
	 * invalidation operation(below) of a protected VM, we will make the page
	 * table entry non-present while still keep its page state information in
	 * the page table entry. So either a donated page is invalidated or not,
	 * it's kept in donated state.
	 *
	 * And the pgtable_free_cb in this current page walker is still walking
	 * the shadow EPT so cannot allow the  __pkvm_host_unshare_guest()
	 * or __pkvm_host_undonate_guest() release shadow EPT table pages. So
	 * we shall get_page befor these APIs called, then put_page to allow
	 * pgtable_free_cb free table pages with correct refcount.
	 *
	 */
	switch (vm->vm_type) {
	case KVM_X86_DEFAULT_VM:
		if (pgt->pgt_ops->pgt_entry_present(ptep)) {
			pgt->mm_ops->get_page(ptep);
			ret = __pkvm_host_unshare_guest(phys, pgt, vaddr, size);
			pgt->mm_ops->put_page(ptep);
			flush_data->flushtlb |= true;
		}
		break;
	case KVM_X86_PROTECTED_VM:
		if (owned_this_page(ptep)) {
			void *virt = pgt->mm_ops->phys_to_virt(phys);

			/*
			 * before return to host, the page previously owned by
			 * protected VM shall be memset to 0 to avoid secret leakage.
			 */
			memset(virt, 0, size);

			pgt->mm_ops->get_page(ptep);
			ret = __pkvm_host_undonate_guest(phys, pgt, vaddr, size);
			pgt->mm_ops->put_page(ptep);
			flush_data->flushtlb |= true;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		pkvm_err("%s failed: ret %d vm_type %d phys 0x%lx GPA 0x%lx size 0x%lx\n",
			 __func__, ret, vm->vm_type, phys, vaddr, size);

	return ret;
}

static int pkvm_shadow_ept_invalidate_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr,
					   int level, void *ptep, struct pgt_flush_data *flush_data,
					   void *arg)
{
	unsigned long phys = pgt->pgt_ops->pgt_entry_to_phys(ptep);
	unsigned long size = pgt->pgt_ops->pgt_level_to_size(level);
	struct pkvm_shadow_vm *vm = sept_to_shadow_vm(pgt);
	int ret = 0;

	if (!pgt->pgt_ops->pgt_entry_present(ptep))
		return 0;

	/*
	 * We need do invalidation for all present page table entry.
	 *
	 * For normal VM, do same as free_leaf, unshare the page from guest,
	 * and do not allow the __pkvm_host_unshare_guest() release shadow
	 * EPT table pages.
	 *
	 * For protected VM, from security consideration, we shall not allow a
	 * donated page to be undonated back to host during ept invalidation,
	 * as it will cause secret leakage during runtime; so we just make the
	 * page table entry not present and keep all the other page entry information
	 * like page state, ADDR, PAGE_SIZE etc.
	 */
	switch(vm->vm_type) {
	case KVM_X86_DEFAULT_VM:
		pgt->mm_ops->get_page(ptep);
		ret = __pkvm_host_unshare_guest(phys, pgt, vaddr, size);
		pgt->mm_ops->put_page(ptep);
		flush_data->flushtlb |= true;
		break;
	case KVM_X86_PROTECTED_VM:
		ept_mk_nopresent(pgt, ptep);
		flush_data->flushtlb |= true;
		break;
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

	pkvm_pgtable_unmap_nosplit(sept, vaddr, size, pkvm_shadow_ept_invalidate_leaf);
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
	struct pkvm_pgtable *sept = &desc->sept;
	struct pkvm_shadow_vm *vm = sept_desc_to_shadow_vm(desc);

	pkvm_spin_lock(&vm->lock);

	if (desc->shadow_eptp) {
		pkvm_pgtable_destroy(sept, pkvm_shadow_ept_free_leaf);
		memset(sept, 0, sizeof(struct pkvm_pgtable));
		desc->shadow_eptp = 0;
	}

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

		if (!pkvm_pgtable_map(sept, gpa, hpa, level_size, 0,
					prot, pkvm_shadow_ept_map_leaf))
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
