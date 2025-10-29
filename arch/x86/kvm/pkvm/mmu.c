// SPDX-License-Identifier: GPL-2.0
#include <asm/kvm_pkvm.h>
#include <asm/pkvm_spinlock.h>
#include "pkvm.h"
#include "mmu.h"
//FIXME: clean up the header files
#include <vmx/pkvm/hyp/pkvm_hyp.h>
#include <vmx/pkvm/hyp/pgtable.h>
#include <vmx/pkvm/hyp/gfp.h>
#include <vmx/pkvm/hyp/ept.h>	//FIXME
#include <vmx/pkvm/hyp/mem_protect.h>
#include <vmx/pkvm/hyp/memory.h>
#include <pkvm.h>

const struct pkvm_pgtable_ops *guest_mmu_pgt_ops;
struct pkvm_pgtable_cap guest_mmu_pgt_cap;

/*
 * FIXME: temporarily reusing the shadow pgt memory pool.
 * Replace it with a memcache supplied by KVM-high.
 */

static void *guest_mmu_zalloc_page(struct pkvm_memcache *mc)
{
	struct hyp_page *p;
	void *page;

	page = hyp_alloc_pages(&shadow_pgt_pool, 0);
	if (page)
		return page;

	if (!mc)
		return NULL;

	page = pop_pkvm_memcache(mc, hyp_phys_to_virt);
	if (!page)
		return page;

	memset(page, 0, PAGE_SIZE);
	p = hyp_virt_to_page(page);
	hyp_set_page_refcounted(p);

	return page;
}

static void guest_mmu_get_page(void *vaddr)
{
	hyp_get_page(&shadow_pgt_pool, vaddr);
}

static void guest_mmu_put_page(void *vaddr)
{
	hyp_put_page(&shadow_pgt_pool, vaddr);
}

static void guest_mmu_flush_tlb(struct pkvm_pgtable *pgt,
				unsigned long addr,
				unsigned long size)
{
	struct pkvm_vm *pkvm_vm = pgt_to_pkvm(pgt);
	int i;

	/*
	 * If we are here because the VM is being torn down and we are
	 * destroying the page table, the vCPUs are already unloaded and
	 * freed, so we cannot request them to flush the TLBs, but we
	 * don't need to flush the TLBs now. A pCPU's TLB will be flushed
	 * next time when loading any VM's vCPU on that pCPU.
	 */
	if (pkvm_vm->is_dying)
		return;

	pkvm_spin_lock(&pkvm_vm->lock);

	for (i = 0; i < to_kvm(pkvm_vm)->created_vcpus; i++) {
		struct pkvm_vcpu *pkvm_vcpu;
		struct kvm_vcpu *vcpu;

		pkvm_vcpu = pkvm_vm->vcpus[i];
		if (!pkvm_vcpu)
			continue;

		vcpu = to_kvm_vcpu(pkvm_vcpu);

		kvm_make_request(KVM_REQ_TLB_FLUSH_CURRENT, vcpu);
		pkvm_kick_vcpu(vcpu);
	}

	pkvm_spin_unlock(&pkvm_vm->lock);
}

static const struct pkvm_mm_ops guest_mmu_mm_ops = {
	.phys_to_virt = pkvm_phys_to_virt,
	.virt_to_phys = pkvm_virt_to_phys,
	.zalloc_page = guest_mmu_zalloc_page,
	.get_page = guest_mmu_get_page,
	.put_page = guest_mmu_put_page,
	.page_count = hyp_page_count,
	.flush_tlb = guest_mmu_flush_tlb,
};

int pkvm_vm_mmu_init(struct pkvm_vm *pkvm_vm)
{
	pkvm_spin_lock_init(&pkvm_vm->mmu_lock);

	return pkvm_pgtable_init(&pkvm_vm->mmu, &guest_mmu_mm_ops,
				 guest_mmu_pgt_ops, &guest_mmu_pgt_cap, true);
}

static bool gpa_range_overlaps_pvmfw(struct kvm *kvm, u64 gpa_start, u64 gpa_end,
				     u64 *gpa_offset, u64 *pvmfw_offset, u64 *size)
{
	struct kvm_protected_vm *pkvm = &kvm->arch.pkvm;
	u64 pvmfw_load_end;
	u64 start, end;

	if (!pkvm_vm_has_pvmfw(kvm))
		return false;

	pvmfw_load_end = pkvm->pvmfw_load_addr + pvmfw_size;

	start = max(gpa_start, pkvm->pvmfw_load_addr);
	end = min(gpa_end, pvmfw_load_end);

	if (end <= start)
		return false;

	*gpa_offset = start - gpa_start;
	*pvmfw_offset = start - pkvm->pvmfw_load_addr;
	*size = end - start;
	return true;
}

static void load_pvmfw(struct kvm *kvm, u64 phys, u64 offset, u64 size)
{
	BUG_ON(!pkvm_is_protected_vm(kvm));
	BUG_ON(offset + size > pvmfw_size);

	memcpy(__pkvm_va(phys), __pkvm_va(pvmfw_base + offset), size);
}

static int guest_mmu_map_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
			      void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	struct pkvm_pgtable_map_data *data = arg;
	u64 size = pgt->pgt_ops->pgt_level_to_size(level);
	struct kvm *kvm = pgt_to_kvm(pgt);
	int ret;

	if (pgt->pgt_ops->pgt_entry_present(ptep)) {
		/*
		 * If the host wants to map the gpa to a different hpa,
		 * it should unmap it first.
		 *
		 * FIXME: we should also check if the host is trying to change
		 * permissions of a mapped page. Currently in such case we return
		 * -EEXIST below, as a result KVM-high will think this is a "good"
		 * error caused by a spurious page fault, and thus will let the
		 * guest retry accessing the page, instead of properly letting the
		 * VMM kill the guest right away. The best way to fix this would be
		 * to implement tracking guest mappings on the host side, so that
		 * the host can check for spurious page faults on its own before
		 * making the hypercall, and thus we won't need to distinguish
		 * these good vs bad errors here in pKVM.
		 */
		if (pgt->pgt_ops->pgt_entry_to_phys(ptep) != data->phys)
			return -EBUSY;

		/*
		 * It is possible that another CPU has just created the same mapping
		 * when multiple CPUs touch the same page simultaneously.
		 * For simplicity check this on pKVM side, since the host doesn't
		 * track guest mappings in any data structure yet.
		 */
		return -EEXIST;
	}

	/*
	 * TODO: use a more suitable API than the existing page state API. Why walk
	 * the page table once again to reach this PTE if we are already at it?
	 * We could combine these 2 layers (MMU and page state API) into one layer.
	 */
	if (pkvm_is_protected_vm(kvm))
		ret = __pkvm_host_donate_guest(data->phys, pgt, vaddr, size,
					       data->prot, data->memcache);
	else
		ret = __pkvm_host_share_guest(data->phys, pgt, vaddr, size,
					      data->prot, data->memcache);
	return ret;
}

static void *admit_host_page(void *arg)
{
	struct pkvm_memcache *host_mc = arg;

	if (!host_mc->nr_pages)
		return NULL;

	if (__pkvm_host_donate_hyp(host_mc->head, PAGE_SIZE)) {
		WARN_ON(1);
		return NULL;
	}

	return pop_pkvm_memcache(host_mc, hyp_phys_to_virt);
}

/* Refill our local memcache by popping pages from the one provided by the host. */
static int refill_memcache(struct pkvm_memcache *mc, unsigned long min_pages,
		    struct pkvm_memcache *host_mc)
{
	struct pkvm_memcache tmp = *host_mc;
	int ret;

	ret =  __topup_pkvm_memcache(mc, min_pages, admit_host_page,
				     hyp_virt_to_phys, &tmp);
	*host_mc = tmp;

	return ret;
}

static int pkvm_refill_mmu_memcache(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);
	struct pkvm_memcache *host_mc;

	host_mc = &pkvm_vcpu->shared_vcpu->arch.pkvm_vcpu.guest_mmu_memcache;

	return refill_memcache(&vcpu->arch.pkvm_vcpu.guest_mmu_memcache,
			       host_mc->nr_pages, host_mc);
}

int pkvm_vm_mmu_map(struct kvm_vcpu *shared_vcpu, u64 gpa, u64 hpa, u64 size, bool writable)
{
	struct pkvm_vcpu *pkvm_vcpu;
	struct pkvm_vm *pkvm_vm;
	struct kvm_vcpu *vcpu;
	struct kvm *kvm;
	u64 gpa_offset, pvmfw_offset, load_size;
	u64 prot;
	int ret;

	pkvm_vcpu = get_pkvm_vcpu_via_shared(shared_vcpu);
	if (!pkvm_vcpu)
		return -EINVAL;

	pkvm_vm = pkvm_vcpu->pkvm_vm;
	kvm = to_kvm(pkvm_vm);
	vcpu = to_kvm_vcpu(pkvm_vcpu);

	if (!writable && pkvm_is_protected_vm(kvm)) {
		ret = -EPERM;
		goto put_pkvm_vcpu;
	}

	/* Top-up our per-vcpu memcache from the host's */
	ret = pkvm_refill_mmu_memcache(pkvm_vcpu);
	if (ret)
		goto put_pkvm_vcpu;

	/* permission bits */
	prot = pkvm_vm->mmu.pgt_ops->pgt_entry_calc_perm(true, writable, true);
	/* memory type bits */
	prot |= kvm_x86_call(get_mt_mask)(vcpu, gpa >> PAGE_SHIFT, false);

	pkvm_spin_lock(&pkvm_vm->mmu_lock);

	ret = pkvm_pgtable_map(&pkvm_vm->mmu, gpa, hpa, size, 0, prot,
			       guest_mmu_map_leaf, &vcpu->arch.pkvm_vcpu.guest_mmu_memcache);

	if (!ret && gpa_range_overlaps_pvmfw(kvm, gpa, gpa + size,
					     &gpa_offset, &pvmfw_offset, &load_size))
		load_pvmfw(kvm, hpa + gpa_offset, pvmfw_offset, load_size);

	pkvm_spin_unlock(&pkvm_vm->mmu_lock);

put_pkvm_vcpu:
	put_pkvm_vcpu(pkvm_vcpu);
	return ret;
}

static int guest_mmu_unmap_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
				void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	unsigned long phys = pgt->pgt_ops->pgt_entry_to_phys(ptep);
	unsigned long size = pgt->pgt_ops->pgt_level_to_size(level);
	int ret;

	if (WARN_ON_ONCE(!pgt->pgt_ops->pgt_entry_present(ptep)))
		return 0;

	pgt->mm_ops->get_page(ptep);
	ret = __pkvm_host_unshare_guest(phys, pgt, vaddr, size);
	pgt->mm_ops->put_page(ptep);

	flush_data->flushtlb = true;

	return ret;
}

int pkvm_vm_mmu_unmap(int vm_handle, u64 gpa, u64 size)
{
	struct pkvm_vm *pkvm_vm;
	int ret;

	pkvm_vm = get_pkvm_vm(vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	if (pkvm_is_protected_vm(to_kvm(pkvm_vm))) {
		ret = -EPERM;
		goto put_pkvm_vm;
	}

	pkvm_spin_lock(&pkvm_vm->mmu_lock);
	ret = pkvm_pgtable_unmap(&pkvm_vm->mmu, gpa, size, guest_mmu_unmap_leaf);
	pkvm_spin_unlock(&pkvm_vm->mmu_lock);

put_pkvm_vm:
	put_pkvm_vm(pkvm_vm);
	return ret;
}

int pkvm_vm_mmu_age(int vm_handle, u64 gpa, u64 size, bool mkold)
{
	struct pkvm_vm *pkvm_vm;
	int ret;

	pkvm_vm = get_pkvm_vm(vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	if (pkvm_is_protected_vm(to_kvm(pkvm_vm))) {
		ret = -EPERM;
		goto put_pkvm_vm;
	}

	pkvm_spin_lock(&pkvm_vm->mmu_lock);
	ret = pkvm_pgtable_test_clear_young(&pkvm_vm->mmu, gpa, size, mkold);
	pkvm_spin_unlock(&pkvm_vm->mmu_lock);

	/*
	 * Do not flush TLB. It will be flushed by the MMU notifier in KVM-high
	 * if needed.
	 */

put_pkvm_vm:
	put_pkvm_vm(pkvm_vm);
	return ret;
}

void pkvm_free_mmu_memcache(struct kvm_vcpu *vcpu, struct pkvm_memcache *teardown_mc)
{
	struct pkvm_memcache *vcpu_mc;
	void *addr;

	vcpu_mc = &vcpu->arch.pkvm_vcpu.guest_mmu_memcache;
	while (vcpu_mc->nr_pages) {
		/* Drain hyp owned memcache and push pages to the teardown memcache */
		addr = pop_pkvm_memcache(vcpu_mc, hyp_phys_to_virt);

		/*
		 * Since pages comes from non-used memcache, there is no need to
		 * zero them before pushing to teardown_mc (which host can
		 * access after donating them back to the host in next step).
		 */
		push_pkvm_memcache(teardown_mc, addr, hyp_virt_to_phys);
		WARN_ON(__pkvm_hyp_donate_host(pkvm_virt_to_phys(addr), PAGE_SIZE, false));
	}
}

static int guest_mmu_free_leaf(struct pkvm_pgtable *pgt, unsigned long vaddr, int level,
			       void *ptep, struct pgt_flush_data *flush_data, void *arg)
{
	unsigned long phys = pgt->pgt_ops->pgt_entry_to_phys(ptep);
	unsigned long size = pgt->pgt_ops->pgt_level_to_size(level);
	struct kvm *kvm = pgt_to_kvm(pgt);

	if (!pgt->pgt_ops->pgt_entry_present(ptep)) {
		/* Guest may only share its pages, not donate them. */
		BUG_ON(pgt->pgt_ops->pgt_entry_mapped(ptep));

		return 0;
	}

	/*
	 * The pgtable_free_cb in this current page walker is still walking
	 * the page table so we cannot allow __pkvm_host_unshare_guest()
	 * or __pkvm_host_undonate_guest() to release the page table pages.
	 * So we shall get_page before calling these APIs, then put_page
	 * to let pgtable_free_cb free table pages with correct refcount.
	 */
	if (pkvm_is_protected_vm(kvm)) {
		void *virt = pgt->mm_ops->phys_to_virt(phys);

		/*
		 * Wipe the protected VM memory page before giving it back
		 * to host, to avoid secrets leakage.
		 */
		memset(virt, 0, size);
		pkvm_clflush_cache_range(virt, size);

		pgt->mm_ops->get_page(ptep);
		BUG_ON(__pkvm_host_undonate_guest(phys, pgt, vaddr, size));
		pgt->mm_ops->put_page(ptep);
	} else {
		pgt->mm_ops->get_page(ptep);
		BUG_ON(__pkvm_host_unshare_guest(phys, pgt, vaddr, size));
		pgt->mm_ops->put_page(ptep);
	}

	return 0;
}

void pkvm_vm_mmu_destroy(struct pkvm_vm *pkvm_vm)
{
	pkvm_pgtable_destroy(&pkvm_vm->mmu, guest_mmu_free_leaf);
}
