/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <asm/kvm_host.h>
#include <asm/kvm_pgtable.h>

#include <kvm/iommu.h>

#include <nvhe/alloc_mgt.h>
#include <nvhe/pkvm.h>

/* alloc/free from atomic pool. */
void *kvm_iommu_donate_pages_atomic(u8 order);
void kvm_iommu_reclaim_pages_atomic(void *p, u8 order);

/* Hypercall handlers */
int kvm_iommu_alloc_domain(pkvm_handle_t domain_id, int type);
int kvm_iommu_free_domain(pkvm_handle_t domain_id);
int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid, u32 pasid_bits,
			 unsigned long flags);
int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid);
size_t kvm_iommu_map_pages(pkvm_handle_t domain_id,
			   unsigned long iova, phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot, unsigned long *mapped);
size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id, unsigned long iova,
			     size_t pgsize, size_t pgcount);
phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova);
bool kvm_iommu_host_dabt_handler(struct kvm_cpu_context *host_ctxt, u64 esr, u64 addr);
size_t kvm_iommu_map_sg(pkvm_handle_t domain, unsigned long iova, struct kvm_iommu_sg *sg,
			unsigned int nent, unsigned int prot);

/* Flags for memory allocation for IOMMU drivers */
#define IOMMU_PAGE_NOCACHE				BIT(0)
void *kvm_iommu_donate_pages(u8 order, int flags);
void kvm_iommu_reclaim_pages(void *p, u8 order);

#define kvm_iommu_donate_page()		kvm_iommu_donate_pages(0, 0)
#define kvm_iommu_donate_page_nc()	kvm_iommu_donate_pages(0, IOMMU_PAGE_NOCACHE)
#define kvm_iommu_reclaim_page(p)	kvm_iommu_reclaim_pages(p, 0)

void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot);
void kvm_iommu_host_stage2_idmap_complete(bool map);
int kvm_iommu_snapshot_host_stage2(struct kvm_hyp_iommu_domain *domain);

int kvm_iommu_dev_block_dma(pkvm_handle_t iommu_id, u32 endpoint_id, bool host_to_guest);

int kvm_iommu_force_free_domain(pkvm_handle_t domain_id, struct pkvm_hyp_vm *vm);
int kvm_iommu_id_to_token(pkvm_handle_t smmu_id, u64 *out_token);

struct kvm_iommu_ops {
	int (*init)(void);
	int (*alloc_domain)(struct kvm_hyp_iommu_domain *domain, int type);
	void (*free_domain)(struct kvm_hyp_iommu_domain *domain);
	struct kvm_hyp_iommu *(*get_iommu_by_id)(pkvm_handle_t iommu_id);
	int (*attach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id, u32 pasid, u32 pasid_bits, unsigned long flags);
	int (*detach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id, u32 pasid);
	int (*map_pages)(struct kvm_hyp_iommu_domain *domain, unsigned long iova,
			 phys_addr_t paddr, size_t pgsize,
			 size_t pgcount, int prot, size_t *total_mapped);
	size_t (*unmap_pages)(struct kvm_hyp_iommu_domain *domain, unsigned long iova,
			      size_t pgsize, size_t pgcount,
			      struct iommu_iotlb_gather *gather);
	phys_addr_t (*iova_to_phys)(struct kvm_hyp_iommu_domain *domain, unsigned long iova);
	void (*iotlb_sync)(struct kvm_hyp_iommu_domain *domain,
			   struct iommu_iotlb_gather *gather);
	bool (*dabt_handler)(struct user_pt_regs *regs, u64 esr, u64 addr);
	void (*host_stage2_idmap)(struct kvm_hyp_iommu_domain *domain,
				  phys_addr_t start, phys_addr_t end, int prot);
	void (*host_stage2_idmap_complete)(bool map);
	int (*suspend)(struct kvm_hyp_iommu *iommu);
	int (*resume)(struct kvm_hyp_iommu *iommu);
	int (*dev_block_dma)(struct kvm_hyp_iommu *iommu, u32 endpoint_id,
			     bool is_host_to_guest);
	int (*get_iommu_token_by_id)(pkvm_handle_t smmu_id, u64 *out_token);
	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
	ANDROID_KABI_RESERVE(3);
	ANDROID_KABI_RESERVE(4);
	ANDROID_KABI_RESERVE(5);
	ANDROID_KABI_RESERVE(6);
	ANDROID_KABI_RESERVE(7);
	ANDROID_KABI_RESERVE(8);
};

int kvm_iommu_init(struct kvm_iommu_ops *ops,
		   struct kvm_hyp_memcache *atomic_mc);
int kvm_iommu_init_device(struct kvm_hyp_iommu *iommu);

int iommu_pkvm_unuse_dma(u64 phys_addr, size_t size);

void kvm_iommu_iotlb_gather_add_page(struct kvm_hyp_iommu_domain *domain,
				     struct iommu_iotlb_gather *gather,
				     unsigned long iova,
				     size_t size);

static inline hyp_spinlock_t *kvm_iommu_get_lock(struct kvm_hyp_iommu *iommu)
{
	/* See struct kvm_hyp_iommu */
	BUILD_BUG_ON(sizeof(iommu->lock) != sizeof(hyp_spinlock_t));
	return (hyp_spinlock_t *)(&iommu->lock);
}

static inline void kvm_iommu_lock_init(struct kvm_hyp_iommu *iommu)
{
	hyp_spin_lock_init(kvm_iommu_get_lock(iommu));
}

static inline void kvm_iommu_lock(struct kvm_hyp_iommu *iommu)
{
	hyp_spin_lock(kvm_iommu_get_lock(iommu));
}

static inline void kvm_iommu_unlock(struct kvm_hyp_iommu *iommu)
{
	hyp_spin_unlock(kvm_iommu_get_lock(iommu));
}

extern struct hyp_mgt_allocator_ops kvm_iommu_allocator_ops;

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
