// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU operations for pKVM
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>
#include <asm/kvm_hypevents.h>

#include <hyp/adjust_pc.h>

#include <kvm/iommu.h>
#include <kvm/device.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>

/* Only one set of ops supported, similary to the kernel */
struct kvm_iommu_ops *kvm_iommu_ops;
void **kvm_hyp_iommu_domains;

/* Hypervisor is non-preemptable, so cur_context can be per cpu. */
DEFINE_PER_CPU(struct pkvm_hyp_vcpu *, __cur_context);
#define cur_context (*this_cpu_ptr(&__cur_context))

/*
 * Common pool that can be used by IOMMU driver to allocate pages.
 */
static struct hyp_pool iommu_host_pool;
static struct hyp_pool iommu_atomic_pool;

DECLARE_PER_CPU(struct kvm_hyp_req, host_hyp_reqs);

/* Protects domains in kvm_hyp_iommu_domains */
static DEFINE_HYP_SPINLOCK(kvm_iommu_domain_lock);

static atomic_t kvm_iommu_idmap_initialized;

static inline void kvm_iommu_idmap_init_done(void)
{
	atomic_set_release(&kvm_iommu_idmap_initialized, 1);
}

static inline bool kvm_iommu_is_ready(void)
{
	return atomic_read_acquire(&kvm_iommu_idmap_initialized) == 1;
}

static int kvm_iommu_refill(struct kvm_hyp_memcache *host_mc)
{
	if (!kvm_iommu_ops)
		return -EINVAL;

	return refill_hyp_pool(&iommu_host_pool, host_mc);
}

static void kvm_iommu_reclaim(struct kvm_hyp_memcache *host_mc, int target)
{
	if (!kvm_iommu_ops)
		return;

	reclaim_hyp_pool(&iommu_host_pool, host_mc, target);
}

static int kvm_iommu_reclaimable(void)
{
	if (!kvm_iommu_ops)
		return 0;

	return hyp_pool_free_pages(&iommu_host_pool);
}

struct hyp_mgt_allocator_ops kvm_iommu_allocator_ops = {
	.refill = kvm_iommu_refill,
	.reclaim = kvm_iommu_reclaim,
	.reclaimable = kvm_iommu_reclaimable,
};

/* Return current vcpu or NULL for host. */
struct pkvm_hyp_vcpu *__get_vcpu(void)
{
	struct kvm_vcpu *vcpu = this_cpu_ptr(&kvm_host_data)->host_ctxt.__hyp_running_vcpu;

	if (vcpu)
		return container_of(vcpu, struct pkvm_hyp_vcpu, vcpu);
	/* Maybe guest is not loaded but we are in teardown context. */
	return cur_context;
}

int iommu_pkvm_unuse_dma(u64 phys_addr, size_t size)
{
	return __pkvm_unuse_dma(phys_addr, size, __get_vcpu());
}

static void *__kvm_iommu_donate_pages(struct hyp_pool *pool, u8 order, int flags)
{
	void *p;
	struct kvm_hyp_req *req = this_cpu_ptr(&host_hyp_reqs);
	int ret;
	size_t size = (1 << order) * PAGE_SIZE;
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();

	p = hyp_alloc_pages(pool, order);
	if (p) {
		/*
		 * If page request is non-cacheable remap it as such
		 * as all pages in the pool are mapped before hand and
		 * assumed to be cacheable.
		 */
		if (flags & IOMMU_PAGE_NOCACHE) {
			/* Make sure all data written before converting to nc. */
			kvm_flush_dcache_to_poc(p, size);

			ret = pkvm_remap_range(p, 1 << order, true);
			if (ret) {
				hyp_put_page(pool, p);
				return NULL;
			}
		}
		return p;
	}

	if (hyp_vcpu) {
		req = pkvm_hyp_req_reserve(hyp_vcpu, KVM_HYP_REQ_TYPE_MEM);
		if (WARN_ON(!req))
			return NULL;
	}

	req->type = KVM_HYP_REQ_TYPE_MEM;
	req->mem.dest = REQ_MEM_DEST_HYP_IOMMU;
	req->mem.sz_alloc = size;
	req->mem.nr_pages = 1;
	return NULL;
}

static void __kvm_iommu_reclaim_pages(struct hyp_pool *pool, void *p, u8 order)
{
	/*
	 * Remap all pages to cacheable, as we don't know, may be use a flag
	 * in the vmemmap or trust the driver to pass the cacheability same
	 * as the allocation on free?
	 */
	pkvm_remap_range(p, 1 << order, false);
	hyp_put_page(pool, p);
}

void *kvm_iommu_donate_pages(u8 order, int flags)
{
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	struct hyp_pool *pool;

	if (hyp_vcpu)
		pool = &pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu)->iommu_pool;
	else
		pool = &iommu_host_pool;

	return __kvm_iommu_donate_pages(pool, order, flags);
}

void kvm_iommu_reclaim_pages(void *p, u8 order)
{
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	struct hyp_pool *pool;

	if (hyp_vcpu)
		pool = &pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu)->iommu_pool;
	else
		pool = &iommu_host_pool;

	__kvm_iommu_reclaim_pages(pool, p, order);
}

void *kvm_iommu_donate_pages_atomic(u8 order)
{
	return __kvm_iommu_donate_pages(&iommu_atomic_pool, order, 0);
}

void kvm_iommu_reclaim_pages_atomic(void *p, u8 order)
{
	__kvm_iommu_reclaim_pages(&iommu_atomic_pool, p, order);
}

static struct kvm_hyp_iommu_domain *
__handle_to_domain(pkvm_handle_t domain_id, bool alloc)
{
	int idx;
	struct kvm_hyp_iommu_domain *domains;

	if (domain_id >= KVM_IOMMU_MAX_DOMAINS)
		return NULL;
	domain_id = array_index_nospec(domain_id, KVM_IOMMU_MAX_DOMAINS);

	idx = domain_id / KVM_IOMMU_DOMAINS_PER_PAGE;
	domains = (struct kvm_hyp_iommu_domain *)READ_ONCE(kvm_hyp_iommu_domains[idx]);
	if (!domains) {
		if (!alloc)
			return NULL;
		domains = kvm_iommu_donate_page();
		if (!domains)
			return NULL;
		/*
		 * handle_to_domain() does not have to be called under a lock,
		 * but even though we allocate a leaf in all cases, it's only
		 * really a valid thing to do under alloc_domain(), which uses a
		 * lock. Races are therefore a host bug and we don't need to be
		 * delicate about it.
		 */
		if (WARN_ON(cmpxchg64_relaxed(&kvm_hyp_iommu_domains[idx], 0,
					      (void *)domains) != 0)) {
			kvm_iommu_reclaim_page(domains);
			return NULL;
		}
	}
	return &domains[domain_id % KVM_IOMMU_DOMAINS_PER_PAGE];
}

static struct kvm_hyp_iommu_domain *
handle_to_domain(pkvm_handle_t domain_id)
{
	return __handle_to_domain(domain_id, true);
}

static int domain_get(struct kvm_hyp_iommu_domain *domain)
{
	int old = atomic_fetch_inc_acquire(&domain->refs);
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	int ret = 0;

	BUG_ON(!old || (old + 1 < 0));

	/* check done after refcount is elevated to avoid race with alloc_domain */
	if (!hyp_vcpu && domain->vm)
		ret = -EPERM;
	if (hyp_vcpu && (domain->vm != pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu)))
		ret = -EPERM;

	if (ret)
		atomic_dec_return_release(&domain->refs);
	return ret;
}

static void domain_put(struct kvm_hyp_iommu_domain *domain)
{
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();

	BUG_ON(!atomic_dec_return_release(&domain->refs));
	WARN_ON(!hyp_vcpu && domain->vm);
	WARN_ON(hyp_vcpu && (domain->vm != pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu)));
}

static int kvm_iommu_init_atomic_pool(struct kvm_hyp_memcache *atomic_mc)
{
	int ret;

	/* atomic_mc is optional. */
	if (!atomic_mc->head)
		return 0;
	ret = hyp_pool_init_empty(&iommu_atomic_pool, 1024 /* order = 10*/);
	if (ret)
		return ret;

	return refill_hyp_pool(&iommu_atomic_pool, atomic_mc);
}

int kvm_iommu_init(struct kvm_iommu_ops *ops,
		   struct kvm_hyp_memcache *atomic_mc)
{
	int ret;
	u64 domain_root_pfn = __hyp_pa(kvm_hyp_iommu_domains) >> PAGE_SHIFT;

	if (!ops ||
	    !ops->init ||
	    !ops->alloc_domain ||
	    !ops->free_domain)
		return 0;

	ret = hyp_pool_init_empty(&iommu_host_pool, 64);
	if (ret)
		return ret;

	ret = __pkvm_host_donate_hyp(domain_root_pfn,
				     KVM_IOMMU_DOMAINS_ROOT_ORDER_NR);
	if (ret)
		return ret;

	kvm_iommu_ops = ops;

	ret = kvm_iommu_init_atomic_pool(atomic_mc);
	if (ret)
		return ret;

	ret = ops->init();
	if (ret)
		goto out_reclaim_domain;

	return ret;

out_reclaim_domain:
	__pkvm_hyp_donate_host(domain_root_pfn, KVM_IOMMU_DOMAINS_ROOT_ORDER_NR);
	return ret;
}

int kvm_iommu_alloc_domain(pkvm_handle_t domain_id, int type)
{
	int ret = -EINVAL;
	struct kvm_hyp_iommu_domain *domain;
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	struct pkvm_hyp_vm *vm;

	/*
	 * Host only has access to the lower half of the domain IDs.
	 * Guest ID space is managed by the hypervisor, so it is trusted.
	 */
	if (!hyp_vcpu && (domain_id >= (KVM_IOMMU_MAX_DOMAINS >> 1)))
		return -EINVAL;

	domain = handle_to_domain(domain_id);
	if (!domain)
		return -ENOMEM;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	if (atomic_read(&domain->refs))
		goto out_unlock;

	domain->domain_id = domain_id;
	ret = kvm_iommu_ops->alloc_domain(domain, type);
	if (ret)
		goto out_unlock;

	if (hyp_vcpu) {
		vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
		domain->vm = vm;
	}
	atomic_set_release(&domain->refs, 1);
out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);
	return ret;
}

int kvm_iommu_free_domain(pkvm_handle_t domain_id)
{
	int ret = 0;
	struct kvm_hyp_iommu_domain *domain;
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	struct pkvm_hyp_vm *vm = NULL;

	domain = handle_to_domain(domain_id);
	if (!domain)
		return -EINVAL;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	if (hyp_vcpu)
		vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	if (WARN_ON(atomic_cmpxchg_acquire(&domain->refs, 1, 0) != 1) || domain->vm != vm) {
		ret = -EINVAL;
		goto out_unlock;
	}

	kvm_iommu_ops->free_domain(domain);

	memset(domain, 0, sizeof(*domain));

out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);

	return ret;
}

int kvm_iommu_force_free_domain(pkvm_handle_t domain_id, struct pkvm_hyp_vm *vm)
{
	struct kvm_hyp_iommu_domain *domain = handle_to_domain(domain_id);

	BUG_ON(!domain);
	cur_context = vm->vcpus[0];

	hyp_spin_lock(&kvm_iommu_domain_lock);
	atomic_set(&domain->refs, 0);
	kvm_iommu_ops->free_domain(domain);
	memset(domain, 0, sizeof(*domain));
	hyp_spin_unlock(&kvm_iommu_domain_lock);
	cur_context = NULL;

	return 0;
}

int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid, u32 pasid_bits)
{
	int ret;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	struct pkvm_hyp_vm *vm = NULL;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	if (hyp_vcpu)
		vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	/*
	 * Make sure device can't transition to/from VMs while in the middle of attach.
	 */
	ret = pkvm_devices_get_context(iommu_id, endpoint_id, vm);
	if (ret)
		return ret;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = kvm_iommu_ops->attach_dev(iommu, domain, endpoint_id, pasid, pasid_bits);
	if (ret)
		domain_put(domain);

out_unlock:
	pkvm_devices_put_context(iommu_id, endpoint_id);
	return ret;
}

int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid)
{
	int ret;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	struct pkvm_hyp_vm *vm = NULL;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	if (hyp_vcpu)
		vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	/* See kvm_iommu_attach_dev(). */
	ret = pkvm_devices_get_context(iommu_id, endpoint_id, vm);
	if (ret)
		return ret;

	domain = handle_to_domain(domain_id);
	if (!domain || atomic_read(&domain->refs) <= 1) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = kvm_iommu_ops->detach_dev(iommu, domain, endpoint_id, pasid);
	if (ret)
		goto out_unlock;

	domain_put(domain);

out_unlock:
	pkvm_devices_put_context(iommu_id, endpoint_id);
	return ret;
}

#define IOMMU_PROT_MASK (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |\
			 IOMMU_NOEXEC | IOMMU_MMIO | IOMMU_PRIV)

size_t kvm_iommu_map_pages(pkvm_handle_t domain_id,
			   unsigned long iova, phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot)
{
	size_t size;
	int ret;
	size_t total_mapped = 0;
	struct kvm_hyp_iommu_domain *domain;

	if (prot & ~IOMMU_PROT_MASK)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova || paddr + size < paddr)
		return 0;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	ret = __pkvm_use_dma(paddr, size, __get_vcpu());
	if (ret)
		return 0;

	kvm_iommu_ops->map_pages(domain, iova, paddr, pgsize, pgcount, prot, &total_mapped);

	pgcount -= total_mapped / pgsize;
	/*
	 * unuse the bits that haven't been mapped yet. The host calls back
	 * either to continue mapping, or to unmap and unuse what's been done
	 * so far.
	 */
	if (pgcount)
		__pkvm_unuse_dma(paddr + total_mapped, pgcount * pgsize, __get_vcpu());

	domain_put(domain);
	return total_mapped;
}

static inline void kvm_iommu_iotlb_sync(struct kvm_hyp_iommu_domain *domain,
					struct iommu_iotlb_gather *iotlb_gather)
{
	if (kvm_iommu_ops->iotlb_sync)
		kvm_iommu_ops->iotlb_sync(domain, iotlb_gather);

	iommu_iotlb_gather_init(iotlb_gather);
}

void kvm_iommu_iotlb_gather_add_page(struct kvm_hyp_iommu_domain *domain,
				     struct iommu_iotlb_gather *gather,
				     unsigned long iova,
				     size_t size)
{
	_iommu_iotlb_add_page(domain, gather, iova, size, kvm_iommu_iotlb_sync);
}

size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id, unsigned long iova,
			     size_t pgsize, size_t pgcount)
{
	size_t size;
	size_t unmapped;
	struct kvm_hyp_iommu_domain *domain;
	struct iommu_iotlb_gather iotlb_gather;

	if (!pgsize || !pgcount)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova)
		return 0;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	iommu_iotlb_gather_init(&iotlb_gather);
	/*
	 * Unlike map, the common code doesn't call the __pkvm_host_unuse_dma,
	 * because this means that we need either walk the table using iova_to_phys
	 * similar to VFIO then unmap and call this function, or unmap leaf (page or
	 * block) at a time, where both might be suboptimal.
	 * For some IOMMU, we can do 2 walks where one only invalidate the pages
	 * and the other decrement the refcount.
	 * As, semantics for this might differ between IOMMUs and it's hard to
	 * standardized, we leave that to the driver.
	 */
	unmapped = kvm_iommu_ops->unmap_pages(domain, iova, pgsize,
						pgcount, &iotlb_gather);
	kvm_iommu_iotlb_sync(domain, &iotlb_gather);

	domain_put(domain);
	return unmapped;
}

phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova)
{
	phys_addr_t phys = 0;
	struct kvm_hyp_iommu_domain *domain;

	domain = handle_to_domain( domain_id);

	if (!domain || domain_get(domain))
		return 0;

	phys = kvm_iommu_ops->iova_to_phys(domain, iova);
	domain_put(domain);
	return phys;
}

bool kvm_iommu_host_dabt_handler(struct kvm_cpu_context *host_ctxt, u64 esr, u64 addr)
{
	bool ret = false;

	if (kvm_iommu_ops && kvm_iommu_ops->dabt_handler)
		ret = kvm_iommu_ops->dabt_handler(host_ctxt, esr, addr);

	if (ret)
		kvm_skip_host_instr();

	return ret;
}

size_t kvm_iommu_map_sg(pkvm_handle_t domain_id, unsigned long iova, struct kvm_iommu_sg *sg,
			unsigned int nent, unsigned int prot)
{
	int ret;
	size_t total_mapped = 0, mapped;
	struct kvm_hyp_iommu_domain *domain;
	phys_addr_t phys;
	size_t size, pgsize, pgcount;
	unsigned int orig_nent = nent;
	struct kvm_iommu_sg *orig_sg = sg;

	if (!kvm_iommu_ops || !kvm_iommu_ops->map_pages)
		return 0;

	if (prot & ~IOMMU_PROT_MASK)
		return 0;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	ret = hyp_pin_shared_mem(sg, sg + nent);
	if (ret)
		goto out_put_domain;

	while (nent--) {
		phys = sg->phys;
		pgsize = sg->pgsize;
		pgcount = sg->pgcount;

		if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
		    iova + size < iova)
			goto out_unpin_sg;

		ret = __pkvm_use_dma(phys, size, __get_vcpu());
		if (ret)
			goto out_unpin_sg;

		mapped = 0;
		kvm_iommu_ops->map_pages(domain, iova, phys, pgsize, pgcount, prot, &mapped);
		total_mapped += mapped;
		phys += mapped;
		iova += mapped;
		/* Might need memory */
		if (mapped != size) {
			__pkvm_unuse_dma(phys, size - mapped, __get_vcpu());
			break;
		}
		sg++;
	}

out_unpin_sg:
	hyp_unpin_shared_mem(orig_sg, orig_sg + orig_nent);
out_put_domain:
	domain_put(domain);
	return total_mapped;
}

int kvm_iommu_dev_block_dma(pkvm_handle_t iommu_id, u32 endpoint_id, bool host_to_guest)
{
	struct kvm_hyp_iommu *iommu;

	if (!kvm_iommu_ops || !kvm_iommu_ops->dev_block_dma)
		return -ENODEV;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -ENOENT;

	return kvm_iommu_ops->dev_block_dma(iommu, endpoint_id, host_to_guest);
}

static int iommu_power_on(struct kvm_power_domain *pd)
{
	struct kvm_hyp_iommu *iommu = container_of(pd, struct kvm_hyp_iommu,
						   power_domain);
	int ret;

	kvm_iommu_lock(iommu);
	ret = kvm_iommu_ops->resume ? kvm_iommu_ops->resume(iommu) : 0;
	if (!ret)
		iommu->power_is_off = false;
	kvm_iommu_unlock(iommu);
	return ret;
}

static int iommu_power_off(struct kvm_power_domain *pd)
{
	struct kvm_hyp_iommu *iommu = container_of(pd, struct kvm_hyp_iommu,
						   power_domain);
	int ret;

	kvm_iommu_lock(iommu);
	iommu->power_is_off = true;
	ret = kvm_iommu_ops->suspend ? kvm_iommu_ops->suspend(iommu) : 0;
	if (!ret)
		iommu->power_is_off = true;
	kvm_iommu_unlock(iommu);
	return ret;
}

static const struct kvm_power_domain_ops iommu_power_ops = {
	.power_on	= iommu_power_on,
	.power_off	= iommu_power_off,
};

/* Must be called from the IOMMU driver per IOMMU */
int kvm_iommu_init_device(struct kvm_hyp_iommu *iommu)
{
	kvm_iommu_lock_init(iommu);

	return pkvm_init_power_domain(&iommu->power_domain, &iommu_power_ops);
}

static inline int pkvm_to_iommu_prot(int prot)
{
	switch (prot) {
	case PKVM_HOST_MEM_PROT:
		return IOMMU_READ | IOMMU_WRITE;
	case PKVM_HOST_MMIO_PROT:
		return IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO;
	case 0:
		return 0;
	default:
		/* We don't understand that, it might cause corruption, so panic. */
		BUG();
	}

	return 0;
}

void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot)
{
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_is_ready())
		return;

	trace_iommu_idmap(start, end, prot);

	domain = __handle_to_domain(KVM_IOMMU_DOMAIN_IDMAP_ID, false);

	kvm_iommu_ops->host_stage2_idmap(domain, start, end, pkvm_to_iommu_prot(prot));
}

void kvm_iommu_host_stage2_idmap_complete(bool map)
{
	if (!kvm_iommu_is_ready() ||
	    !kvm_iommu_ops->host_stage2_idmap_complete)
		return;

	trace_iommu_idmap_complete(map);
	kvm_iommu_ops->host_stage2_idmap_complete(map);
}

static int __snapshot_host_stage2(const struct kvm_pgtable_visit_ctx *ctx,
				  enum kvm_pgtable_walk_flags visit)
{
	u64 start = ctx->addr;
	kvm_pte_t pte = *ctx->ptep;
	u32 level = ctx->level;
	struct kvm_hyp_iommu_domain *domain = ctx->arg;
	u64 end = start + kvm_granule_size(level);
	int prot = IOMMU_READ | IOMMU_WRITE;

	if (!addr_is_memory(start))
		prot |= IOMMU_MMIO;

	if (!pte || kvm_pte_valid(pte))
		kvm_iommu_ops->host_stage2_idmap(domain, start, end, prot);

	return 0;
}

int kvm_iommu_snapshot_host_stage2(struct kvm_hyp_iommu_domain *domain)
{
	int ret;
	struct kvm_pgtable_walker walker = {
		.cb	= __snapshot_host_stage2,
		.flags	= KVM_PGTABLE_WALK_LEAF,
		.arg = domain,
	};
	struct kvm_pgtable *pgt = &host_mmu.pgt;

	hyp_spin_lock(&host_mmu.lock);
	ret = kvm_pgtable_walk(pgt, 0, BIT(pgt->ia_bits), &walker);
	/* Start receiving calls to host_stage2_idmap. */
	if (!ret)
		kvm_iommu_idmap_init_done();
	hyp_spin_unlock(&host_mmu.lock);

	return ret;
}
