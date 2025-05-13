/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_ARM_SMMU_V3_H
#define __KVM_ARM_SMMU_V3_H

#include <asm/arm-smmu-v3-common.h>
#include <asm/kvm_asm.h>
#include <kvm/iommu.h>

/*
 * Parameters from the trusted host:
 * @mmio_addr		base address of the SMMU registers
 * @mmio_size		size of the registers resource
 *
 * Other members are filled and used at runtime by the SMMU driver.
 */
struct hyp_arm_smmu_v3_device {
	struct kvm_hyp_iommu	iommu;
	phys_addr_t		mmio_addr;
	size_t			mmio_size;
	unsigned long		features;

	void __iomem		*base;
	u32			cmdq_prod;
	u64			*cmdq_base;
	size_t			cmdq_log2size;
	/* strtab_cfg.l2.l2ptrs is not used, instead computed from L1 */
	struct arm_smmu_strtab_cfg strtab_cfg;
	size_t			oas;
	size_t			ias;
	size_t			pgsize_bitmap;
	size_t			ssid_bits;
};

extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count);
#define kvm_hyp_arm_smmu_v3_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count)

extern struct hyp_arm_smmu_v3_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus);
#define kvm_hyp_arm_smmu_v3_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus)

enum kvm_arm_smmu_domain_type {
	KVM_ARM_SMMU_DOMAIN_BYPASS = KVM_IOMMU_DOMAIN_IDMAP_TYPE,
	KVM_ARM_SMMU_DOMAIN_ANY = KVM_IOMMU_DOMAIN_ANY_TYPE,
	KVM_ARM_SMMU_DOMAIN_S1,
	KVM_ARM_SMMU_DOMAIN_S2,
	KVM_ARM_SMMU_DOMAIN_MAX,
};

#endif /* __KVM_ARM_SMMU_V3_H */
