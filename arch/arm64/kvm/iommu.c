// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <kvm/iommu.h>

#include <linux/arm-smccc.h>
#include <linux/kvm_host.h>

#define kvm_call_hyp_nvhe_mc(...)					\
({									\
	struct arm_smccc_res __res;					\
	do {								\
		__res = kvm_call_hyp_nvhe_smccc(__VA_ARGS__);		\
	} while (__res.a1 && !kvm_iommu_topup_memcache(&__res, GFP_KERNEL));\
	__res.a1;							\
})


static int kvm_iommu_topup_memcache(struct arm_smccc_res *res, gfp_t gfp)
{
	struct kvm_hyp_req req;

	hyp_reqs_smccc_decode(res, &req);

	if ((res->a1 == -ENOMEM) && (req.type != KVM_HYP_REQ_TYPE_MEM)) {
		/*
		 * There is no way for drivers to populate hyp_alloc requests,
		 * so -ENOMEM + no request indicates that.
		 */
		return __pkvm_topup_hyp_alloc(1);
	} else if (req.type != KVM_HYP_REQ_TYPE_MEM) {
		return -EBADE;
	}

	if (req.mem.dest == REQ_MEM_DEST_HYP_IOMMU) {
		return __pkvm_topup_hyp_alloc_mgt_gfp(HYP_ALLOC_MGT_IOMMU_ID,
						      req.mem.nr_pages,
						      req.mem.sz_alloc,
						      gfp);
	} else if (req.mem.dest == REQ_MEM_DEST_HYP_ALLOC) {
		/* Fill hyp alloc*/
		return __pkvm_topup_hyp_alloc(req.mem.nr_pages);
	}

	pr_err("Bogus mem request");
	return -EBADE;
}

struct kvm_iommu_driver *iommu_driver;
extern struct kvm_iommu_ops *kvm_nvhe_sym(kvm_iommu_ops);

int kvm_iommu_register_driver(struct kvm_iommu_driver *kern_ops)
{
	if (WARN_ON(!kern_ops))
		return -EINVAL;

	/*
	 * Paired with smp_load_acquire(&iommu_driver)
	 * Ensure memory stores happening during a driver
	 * init are observed before executing kvm iommu callbacks.
	 */
	return cmpxchg_release(&iommu_driver, NULL, kern_ops) ? -EBUSY : 0;
}
EXPORT_SYMBOL(kvm_iommu_register_driver);

int kvm_iommu_init_hyp(struct kvm_iommu_ops *hyp_ops,
		       struct kvm_hyp_memcache *atomic_mc)
{
	if (!hyp_ops)
		return -EINVAL;

	return kvm_call_hyp_nvhe(__pkvm_iommu_init, hyp_ops,
				 atomic_mc->head, atomic_mc->nr_pages);
}
EXPORT_SYMBOL(kvm_iommu_init_hyp);

int kvm_iommu_init_driver(void)
{
	if (!smp_load_acquire(&iommu_driver) || !iommu_driver->get_iommu_id_by_of) {
		kvm_err("pKVM enabled without an IOMMU driver, do not run confidential workloads in virtual machines\n");
		return -ENODEV;
	}

	kvm_hyp_iommu_domains = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
				get_order(KVM_IOMMU_DOMAINS_ROOT_SIZE));
	if (!kvm_hyp_iommu_domains)
		return -ENOMEM;

	kvm_hyp_iommu_domains = kern_hyp_va(kvm_hyp_iommu_domains);

	return iommu_driver->init_driver();
}
EXPORT_SYMBOL(kvm_iommu_init_driver);

void kvm_iommu_remove_driver(void)
{
	if (smp_load_acquire(&iommu_driver))
		iommu_driver->remove_driver();
}


pkvm_handle_t kvm_get_iommu_id_by_of(struct device_node *np)
{
	if (!iommu_driver)
		return 0;

	return iommu_driver->get_iommu_id_by_of(np);
}

static pkvm_handle_t kvm_get_iommu_id(struct device *dev)
{
	return kvm_get_iommu_id_by_of(dev_of_node(dev));
}

int pkvm_iommu_suspend(struct device *dev)
{
	int device_id = kvm_get_iommu_id(dev);

	return kvm_call_hyp_nvhe(__pkvm_host_hvc_pd, device_id, 0);
}
EXPORT_SYMBOL(pkvm_iommu_suspend);

int pkvm_iommu_resume(struct device *dev)
{
	int device_id = kvm_get_iommu_id(dev);

	return kvm_call_hyp_nvhe(__pkvm_host_hvc_pd, device_id, 1);
}
EXPORT_SYMBOL(pkvm_iommu_resume);

int kvm_iommu_share_hyp_sg(struct kvm_iommu_sg *sg, unsigned int nents)
{
	size_t nr_pages = PAGE_ALIGN(sizeof(*sg) * nents) >> PAGE_SHIFT;
	phys_addr_t sg_pfn = virt_to_phys(sg) >> PAGE_SHIFT;
	int i;
	int ret;

	for (i = 0 ; i < nr_pages ; ++i) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_share_hyp, sg_pfn + i);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(kvm_iommu_share_hyp_sg);

int kvm_iommu_unshare_hyp_sg(struct kvm_iommu_sg *sg, unsigned int nents)
{
	size_t nr_pages = PAGE_ALIGN(sizeof(*sg) * nents) >> PAGE_SHIFT;
	phys_addr_t sg_pfn = virt_to_phys(sg) >> PAGE_SHIFT;
	int i;
	int ret;

	for (i = 0 ; i < nr_pages ; ++i) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp, sg_pfn + i);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(kvm_iommu_unshare_hyp_sg);

int kvm_iommu_device_num_ids(struct device *dev)
{
	if (iommu_driver->get_device_iommu_num_ids)
		return iommu_driver->get_device_iommu_num_ids(dev);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_iommu_device_num_ids);

int kvm_iommu_device_id(struct device *dev, u32 idx,
			pkvm_handle_t *out_iommu, u32 *out_sid)
{
	if (iommu_driver->get_device_iommu_id)
		return iommu_driver->get_device_iommu_id(dev, idx, out_iommu, out_sid);
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(kvm_iommu_device_id);

int kvm_iommu_guest_alloc_mc(struct kvm_hyp_memcache *mc, u32 pgsize, u32 nr_pages)
{
	u8 order = get_order(pgsize);

	/* Driver might have dedicated allocator especially if it needs large pages. */
	if (iommu_driver && iommu_driver->guest_alloc && iommu_driver->guest_free)
		return __topup_hyp_memcache(mc, nr_pages, iommu_driver->guest_alloc,
					    kvm_host_pa, 0, order);

	return topup_hyp_memcache(mc, nr_pages, order);
}

void kvm_iommu_guest_free_mc(struct kvm_hyp_memcache *mc)
{
	if (iommu_driver && iommu_driver->guest_alloc && iommu_driver->guest_free)
		__free_hyp_memcache(mc, iommu_driver->guest_free,
				    kvm_host_va, 0);
	else
		free_hyp_memcache(mc);
}

/* Hypercall abstractions exposed to kernel IOMMU drivers */
int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 unsigned int endpoint, unsigned int pasid,
			 unsigned int ssid_bits, unsigned long flags)
{
	return kvm_call_hyp_nvhe_mc(__pkvm_host_iommu_attach_dev, iommu_id, domain_id,
				    endpoint, pasid, ssid_bits, flags);
}
EXPORT_SYMBOL(kvm_iommu_attach_dev);

int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 unsigned int endpoint, unsigned int pasid)
{
	return kvm_call_hyp_nvhe(__pkvm_host_iommu_detach_dev, iommu_id, domain_id,
				endpoint, pasid);
}
EXPORT_SYMBOL(kvm_iommu_detach_dev);

int kvm_iommu_alloc_domain(pkvm_handle_t domain_id, int type)
{
	return kvm_call_hyp_nvhe_mc(__pkvm_host_iommu_alloc_domain,
				    domain_id, type);
}
EXPORT_SYMBOL(kvm_iommu_alloc_domain);

int kvm_iommu_free_domain(pkvm_handle_t domain_id)
{
	return kvm_call_hyp_nvhe(__pkvm_host_iommu_free_domain, domain_id);
}
EXPORT_SYMBOL(kvm_iommu_free_domain);

int kvm_iommu_map_pages(pkvm_handle_t domain_id, unsigned long iova,
			phys_addr_t paddr, size_t pgsize, size_t pgcount,
			int prot, gfp_t gfp, size_t *total_mapped)
{
	size_t mapped;
	size_t size = pgsize * pgcount;
	struct arm_smccc_res res;

	do {
		res = kvm_call_hyp_nvhe_smccc(__pkvm_host_iommu_map_pages, domain_id,
					      iova, paddr, pgsize, pgcount, prot);
		mapped = res.a1;
		iova += mapped;
		paddr += mapped;
		WARN_ON(mapped % pgsize);
		WARN_ON(mapped > pgcount * pgsize);
		pgcount -= mapped / pgsize;
		*total_mapped += mapped;
	} while (*total_mapped < size && !kvm_iommu_topup_memcache(&res, gfp));
	if (*total_mapped < size)
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL(kvm_iommu_map_pages);

size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id, unsigned long iova,
			     size_t pgsize, size_t pgcount)
{
	size_t unmapped;
	size_t total_unmapped = 0;
	size_t size = pgsize * pgcount;
	struct arm_smccc_res res;

	do {
		res = kvm_call_hyp_nvhe_smccc(__pkvm_host_iommu_unmap_pages,
					      domain_id, iova, pgsize, pgcount);
		unmapped = res.a1;
		total_unmapped += unmapped;
		iova += unmapped;
		WARN_ON(unmapped % pgsize);
		pgcount -= unmapped / pgsize;

		/*
		 * The page table driver can unmap less than we asked for. If it
		 * didn't unmap anything at all, then it either reached the end
		 * of the range, or it needs a page in the memcache to break a
		 * block mapping.
		 */
	} while (total_unmapped < size &&
		 (unmapped || !kvm_iommu_topup_memcache(&res, GFP_ATOMIC)));

	return total_unmapped;

}
EXPORT_SYMBOL(kvm_iommu_unmap_pages);

phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova)
{
	return kvm_call_hyp_nvhe(__pkvm_host_iommu_iova_to_phys, domain_id, iova);
}
EXPORT_SYMBOL(kvm_iommu_iova_to_phys);

size_t kvm_iommu_map_sg(pkvm_handle_t domain_id, struct kvm_iommu_sg *sg,
			unsigned long iova, unsigned int nent,
			unsigned int prot, gfp_t gfp)
{
	size_t mapped, total_mapped = 0;
	struct arm_smccc_res res;

	do {
		res = kvm_call_hyp_nvhe_smccc(__pkvm_host_iommu_map_sg,
					      domain_id, iova, sg, nent, prot);
		mapped = res.a1;
		iova += mapped;
		total_mapped += mapped;
		/* Skip mapped */
		while (mapped) {
			if (mapped < (sg->pgsize * sg->pgcount)) {
				sg->phys += mapped;
				sg->pgcount -= mapped / sg->pgsize;
				mapped = 0;
			} else {
				mapped -= sg->pgsize * sg->pgcount;
				sg++;
				nent--;
			}
		}

		kvm_iommu_topup_memcache(&res, gfp);
	} while (nent);

	return total_mapped;
}
EXPORT_SYMBOL(kvm_iommu_map_sg);
