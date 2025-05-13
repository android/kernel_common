/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_IOMMU_H
#define __KVM_IOMMU_H

#include <asm/kvm_host.h>
#include <kvm/power_domain.h>
#include <linux/io-pgtable.h>

/*
 * Domain ID for identity mapped domain that the host can attach
 * to get the same mapping available to the CPU page table.
 */
#define KVM_IOMMU_DOMAIN_IDMAP_ID		0

/* Used in alloc_domain type argument. */
#define KVM_IOMMU_DOMAIN_IDMAP_TYPE		0
/* Typically used for guests, as they don't know IOMMU topology. */
#define KVM_IOMMU_DOMAIN_ANY_TYPE		1

#define KVM_IOMMU_DOMAIN_NR_START		(KVM_IOMMU_DOMAIN_IDMAP_ID + 1)

struct kvm_hyp_iommu_domain {
	atomic_t		refs;
	pkvm_handle_t		domain_id;
	void			*priv;
	void			*vm;
	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
};

extern void **kvm_nvhe_sym(kvm_hyp_iommu_domains);
#define kvm_hyp_iommu_domains kvm_nvhe_sym(kvm_hyp_iommu_domains)

/*
 * At the moment the number of domains is limited to 2^16
 * In practice we're rarely going to need a lot of domains. To avoid allocating
 * a large domain table, we use a two-level table, indexed by domain ID. With
 * 4kB pages and 16-bytes domains, the leaf table contains 256 domains, and the
 * root table 256 pointers. With 64kB pages, the leaf table contains 4096
 * domains and the root table 16 pointers. In this case, or when using 8-bit
 * VMIDs, it may be more advantageous to use a single level. But using two
 * levels allows to easily extend the domain size.
 */
#define KVM_IOMMU_MAX_DOMAINS	(1 << 16)

/* Number of entries in the level-2 domain table */
#define KVM_IOMMU_DOMAINS_PER_PAGE \
	(PAGE_SIZE / sizeof(struct kvm_hyp_iommu_domain))

/* Number of entries in the root domain table */
#define KVM_IOMMU_DOMAINS_ROOT_ENTRIES \
	(KVM_IOMMU_MAX_DOMAINS / KVM_IOMMU_DOMAINS_PER_PAGE)

#define KVM_IOMMU_DOMAINS_ROOT_SIZE \
	(KVM_IOMMU_DOMAINS_ROOT_ENTRIES * sizeof(void *))

#define KVM_IOMMU_DOMAINS_ROOT_ORDER_NR	\
	(1 << get_order(KVM_IOMMU_DOMAINS_ROOT_SIZE))

struct kvm_hyp_iommu {
	u32				lock;   /* lock size verified in kvm_iommu_get_lock.  */
	struct kvm_power_domain		power_domain;
	bool				power_is_off;
	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
	ANDROID_KABI_RESERVE(3);
	ANDROID_KABI_RESERVE(4);
};

#endif /* __KVM_IOMMU_H */
