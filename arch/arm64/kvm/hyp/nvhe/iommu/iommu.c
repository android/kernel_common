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

phys_addr_t cma_base;
size_t cma_size;

#define MAX_BLOCK_POOLS 16

/*
 * Common pool that can be used by IOMMU driver to allocate pages.
 */
static struct hyp_pool iommu_system_pool;
static struct hyp_pool iommu_block_pools[MAX_BLOCK_POOLS];
static struct hyp_pool iommu_atomic_pool;

/*
 * hyp_pool->lock is dropped multiple times during a block_pool reclaim. We then
 * need another global lock to serialize that operation with an allocation.
 */
static DEFINE_HYP_SPINLOCK(__block_pools_lock);
static bool __block_pools_available;

static const u8 pmd_order = PMD_SHIFT - PAGE_SHIFT;

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

static bool kvm_iommu_donate_from_cma(phys_addr_t phys, unsigned long order)
{
	phys_addr_t end = phys + PAGE_SIZE * (1 << order);

	if (end <= phys)
		return false;

	if (order != pmd_order)
		return false;

	if (!IS_ALIGNED(phys, PMD_SIZE))
		return false;

	if (phys < cma_base || end > cma_base + cma_size)
		return false;

	return true;
}

static struct hyp_pool *__get_empty_block_pool(phys_addr_t phys)
{
	int p;

	for (p = 0; p < MAX_BLOCK_POOLS; p++) {
		struct hyp_pool *pool = &iommu_block_pools[p];

		if (pool->max_order)
			continue;

		if (hyp_pool_init(pool, hyp_phys_to_pfn(phys), 1 << pmd_order, 0))
			return NULL;

		WRITE_ONCE(__block_pools_available, 1);

		return pool;
	}

	return NULL;
}

static void __repudiate_host_page(void *addr, unsigned long order,
				  struct kvm_hyp_memcache *host_mc)
{
	push_hyp_memcache(host_mc, addr, hyp_virt_to_phys, order);
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(addr), 1 << order));
}

int kvm_iommu_refill(struct kvm_hyp_memcache *host_mc)
{
	struct kvm_hyp_memcache tmp_mc = *host_mc;

	if (!kvm_iommu_ops)
		return -EINVAL;

	while (tmp_mc.nr_pages) {
		unsigned long order = FIELD_GET(~PAGE_MASK, tmp_mc.head);
		phys_addr_t phys = tmp_mc.head & PAGE_MASK;
		struct hyp_pool *pool = &iommu_system_pool;
		u64 nr_pages;
		void *addr;

		if (check_shl_overflow(1UL, order, &nr_pages) ||
		    !IS_ALIGNED(phys, PAGE_SIZE << order))
			return -EINVAL;

		addr = admit_host_page(&tmp_mc, order);
		if (!addr)
			return -EINVAL;
		*host_mc = tmp_mc;

		if (kvm_iommu_donate_from_cma(phys, order)) {
			hyp_spin_lock(&__block_pools_lock);
			pool = __get_empty_block_pool(phys);
			hyp_spin_unlock(&__block_pools_lock);
			if (!pool) {
				__repudiate_host_page(addr, order, &tmp_mc);
				*host_mc = tmp_mc;
				return -EBUSY;
			}
		} else {
			hyp_virt_to_page(addr)->order = order;
			hyp_set_page_refcounted(hyp_virt_to_page(addr));
			hyp_put_page(pool, addr);
		}
	}

	return 0;
}

void kvm_iommu_reclaim(struct kvm_hyp_memcache *host_mc, int target)
{
	unsigned long prev_nr_pages = host_mc->nr_pages;
	unsigned long block_pages = 1 << pmd_order;
	int p = 0;

	if (!kvm_iommu_ops)
		return;

	reclaim_hyp_pool(&iommu_system_pool, host_mc, target);

	target -= host_mc->nr_pages - prev_nr_pages;

	while (target > block_pages && p < MAX_BLOCK_POOLS) {
		struct hyp_pool *pool = &iommu_block_pools[p];

		hyp_spin_lock(&__block_pools_lock);

		if (hyp_pool_free_pages(pool) == block_pages) {
			reclaim_hyp_pool(pool, host_mc, block_pages);
			hyp_pool_init_empty(pool, 1);
			target -= block_pages;
		}

		hyp_spin_unlock(&__block_pools_lock);
		p++;
	}
}

int kvm_iommu_reclaimable(void)
{
	unsigned long reclaimable = 0;
	int p;

	if (!kvm_iommu_ops)
		return 0;

	reclaimable += hyp_pool_free_pages(&iommu_system_pool);

	/*
	 * This also accounts for blocks, allocated from the CMA region. This is
	 * not exactly what the shrinker wants... but we need to have a way to
	 * report this memory to the host.
	 */

	for (p = 0; p < MAX_BLOCK_POOLS; p++) {
		unsigned long __free_pages = hyp_pool_free_pages(&iommu_block_pools[p]);

		if (__free_pages == 1 << pmd_order)
			reclaimable += __free_pages;
	}

	return reclaimable;
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

static void *__kvm_iommu_alloc_pages(u8 order, struct hyp_pool **pool)
{
	static int last_block_pool;
	void *p;
	int i;

	if (!READ_ONCE(__block_pools_available))
		goto from_system_pool;

	hyp_spin_lock(&__block_pools_lock);

	i = last_block_pool;
	do {
		*pool = &iommu_block_pools[i];
		p = hyp_alloc_pages(*pool, order);
		if (p) {
			last_block_pool = i;
			hyp_spin_unlock(&__block_pools_lock);
			return p;
		}

		if (++i >= MAX_BLOCK_POOLS)
			i = 0;
	} while (i != last_block_pool);

	WRITE_ONCE(__block_pools_available, 0);

	hyp_spin_unlock(&__block_pools_lock);

from_system_pool:
	*pool = &iommu_system_pool;
	return hyp_alloc_pages(*pool, order);
}

void *kvm_iommu_donate_pages(u8 order, int flags)
{
	struct kvm_hyp_req *req = this_cpu_ptr(&host_hyp_reqs);
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	size_t size = (1 << order) * PAGE_SIZE;
	struct hyp_pool *pool;
	void *p;

	if (hyp_vcpu) {
		pool = &pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu)->iommu_pool;
		p = hyp_alloc_pages(pool, order);
	} else {
		p = __kvm_iommu_alloc_pages(order, &pool);
	}

	if (p) {
		/*
		 * If page request is non-cacheable remap it as such
		 * as all pages in the pool are mapped before hand and
		 * assumed to be cacheable.
		 */
		if (flags & IOMMU_PAGE_NOCACHE) {
			int ret;

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

void kvm_iommu_reclaim_pages(void *p, u8 order)
{
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	phys_addr_t phys = hyp_virt_to_phys(p);
	int i;

	if (hyp_vcpu) {
		__kvm_iommu_reclaim_pages(&pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu)->iommu_pool, p, order);
		return;
	}

	if (phys < cma_base || phys >= (cma_base + cma_size)) {
		__kvm_iommu_reclaim_pages(&iommu_system_pool, p, order);
		return;
	}

	hyp_spin_lock(&__block_pools_lock);

	for (i = 0; i < MAX_BLOCK_POOLS; i++) {
		struct hyp_pool *pool = &iommu_block_pools[i];

		if (!pool->max_order)
			continue;

		if (phys >= pool->range_start && phys < pool->range_end) {
			__kvm_iommu_reclaim_pages(pool, p, order);
			hyp_spin_unlock(&__block_pools_lock);
			return;
		}
	}

	hyp_spin_lock(&__block_pools_lock);

	WARN_ON(1);
}

void *kvm_iommu_donate_pages_atomic(u8 order)
{
	return hyp_alloc_pages(&iommu_atomic_pool, order);
}

void kvm_iommu_reclaim_pages_atomic(void *p, u8 order)
{
	hyp_put_page(&iommu_atomic_pool, p);
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
	int i, ret;
	u64 domain_root_pfn = __hyp_pa(kvm_hyp_iommu_domains) >> PAGE_SHIFT;

	if (!ops ||
	    !ops->init ||
	    !ops->alloc_domain ||
	    !ops->free_domain ||
	    !ops->get_iommu_by_id)
		return -ENODEV;

	ret = hyp_pool_init_empty(&iommu_system_pool, 64);
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

	for (i = 0; i < MAX_BLOCK_POOLS; i++) {
		ret = hyp_pool_init_empty(&iommu_block_pools[i], 1);
		if (ret)
			return ret;
	}


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
			 u32 endpoint_id, u32 pasid, u32 pasid_bits,
			 unsigned long flags)
{
	int ret;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;
	struct pkvm_hyp_vcpu *hyp_vcpu = __get_vcpu();
	struct pkvm_hyp_vm *vm = NULL;

	if (!kvm_iommu_ops || !kvm_iommu_ops->attach_dev)
		return -ENODEV;

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

	ret = kvm_iommu_ops->attach_dev(iommu, domain, endpoint_id, pasid, pasid_bits, flags);
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

	if (!kvm_iommu_ops || !kvm_iommu_ops->detach_dev)
		return -ENODEV;

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
			   size_t pgcount, int prot, unsigned long *mapped)
{
	size_t size;
	int ret;
	size_t total_mapped = 0;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->map_pages)
		return -ENODEV;

	*mapped = 0;

	if (prot & ~IOMMU_PROT_MASK)
		return -EOPNOTSUPP;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova || paddr + size < paddr)
		return -E2BIG;

	if (domain_id == KVM_IOMMU_DOMAIN_IDMAP_ID)
		return -EINVAL;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return -ENOENT;

	ret = __pkvm_use_dma(paddr, size, __get_vcpu());
	if (ret)
		goto out_put_domain;

	ret = kvm_iommu_ops->map_pages(domain, iova, paddr, pgsize, pgcount,
				       prot, &total_mapped);

	pgcount -= total_mapped / pgsize;
	/*
	 * unuse the bits that haven't been mapped yet. The host calls back
	 * either to continue mapping, or to unmap and unuse what's been done
	 * so far.
	 */
	if (pgcount)
		__pkvm_unuse_dma(paddr + total_mapped, pgcount * pgsize, __get_vcpu());

	*mapped = total_mapped;

out_put_domain:
	domain_put(domain);
	/* Mask -ENOMEM, as it's passed as a request. */
	return ret == -ENOMEM ? 0 : ret;
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

	if (!kvm_iommu_ops || !kvm_iommu_ops->unmap_pages)
		return -ENODEV;

	if (!pgsize || !pgcount)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova)
		return 0;

	if (domain_id == KVM_IOMMU_DOMAIN_IDMAP_ID)
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

	if (!kvm_iommu_ops || !kvm_iommu_ops->iova_to_phys)
		return -ENODEV;

	if (domain_id == KVM_IOMMU_DOMAIN_IDMAP_ID)
		return iova;

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
		ret = kvm_iommu_ops->dabt_handler(&host_ctxt->regs, esr, addr);

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

int kvm_iommu_id_to_token(pkvm_handle_t id, u64 *out_token)
{
	if (!kvm_iommu_ops || !kvm_iommu_ops->get_iommu_token_by_id)
		return -ENODEV;
	return kvm_iommu_ops->get_iommu_token_by_id(id, out_token);
}
