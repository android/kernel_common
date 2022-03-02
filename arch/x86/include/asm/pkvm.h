/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _ASM_X86_PKVM_H
#define _ASM_X86_PKVM_H

#include <asm/kvm_para.h>
#include <asm/io.h>

/* PKVM Hypercalls */
#define PKVM_HC_INIT_FINALISE		1
#define PKVM_HC_INIT_SHADOW_VM		2
#define PKVM_HC_INIT_SHADOW_VCPU	3
#define PKVM_HC_TEARDOWN_SHADOW_VM	4
#define PKVM_HC_TEARDOWN_SHADOW_VCPU	5
#define PKVM_HC_MMIO_ACCESS		6
#define PKVM_HC_TLB_REMOTE_FLUSH_RANGE	8

/* PKVM provided hypercalls for guest use. */
#define PKVM_GHC_NUM(x)	(x + KVM_HC_PKVM_OP)

#define PKVM_GHC_SHARE_MEM		PKVM_GHC_NUM(1)
#define PKVM_GHC_UNSHARE_MEM		PKVM_GHC_NUM(2)

/* 15bits for PASID */
#define PKVM_MAX_PASID	0x8000

#ifdef CONFIG_PKVM_INTEL
DECLARE_PER_CPU_READ_MOSTLY(bool, pkvm_enabled);

static inline u64 pkvm_readq(void __iomem *reg, unsigned long reg_phys,
			     unsigned long offset)
{
	if (likely(this_cpu_read(pkvm_enabled)))
		return (u64)kvm_hypercall3(PKVM_HC_MMIO_ACCESS, true,
					   sizeof(u64), reg_phys + offset);
	else
		return readq(reg + offset);
}

static inline u32 pkvm_readl(void __iomem *reg, unsigned long reg_phys,
			     unsigned long offset)
{
	if (likely(this_cpu_read(pkvm_enabled)))
		return (u32)kvm_hypercall3(PKVM_HC_MMIO_ACCESS, true,
					   sizeof(u32), reg_phys + offset);
	else
		return readl(reg + offset);
}

static inline void pkvm_writeq(void __iomem *reg, unsigned long reg_phys,
			       unsigned long offset, u64 val)
{
	if (likely(this_cpu_read(pkvm_enabled)))
		kvm_hypercall4(PKVM_HC_MMIO_ACCESS, false, sizeof(u64),
			       reg_phys + offset, val);
	else
		writeq(val, reg + offset);
}

static inline void pkvm_writel(void __iomem *reg, unsigned long reg_phys,
			       unsigned long offset, u32 val)
{
	if (likely(this_cpu_read(pkvm_enabled)))
		kvm_hypercall4(PKVM_HC_MMIO_ACCESS, false, sizeof(u32),
			       reg_phys + offset, (u64)val);
	else
		writel(val, reg + offset);
}

static inline void pkvm_update_iommu_virtual_caps(u64 *cap, u64 *ecap)
{
	if (cap)
		/*
		 * Set caching mode as linux OS will runs in a VM
		 * with controlling a virtual IOMMU device emulated
		 * by pkvm.
		 */
		*cap |= 1 << 7;

	if (ecap)
		/*
		 * Some IOMMU capabilities cannot be directly used by the linux
		 * IOMMU driver after the linux is deprivileged, which is because after
		 * deprivileging, pkvm IOMMU driver will control the physical IOMMU and
		 * it is designed to use physical IOMMU in two ways for better performance
		 * and simpler implementation:
		 * 1. using nested translation with the first-level from the deprivileged
		 * linux IOMMU driver and EPT as second-level.
		 * 2. using second-level only translation with EPT.
		 * The linux IOMMU driver then uses an virtual IOMMU device emulated by
		 * pkvm IOMMU driver.
		 *
		 * Way#1 and way#2 can only support the linux IOMMU driver works in
		 * first-level translation mode or HW pass-through mode. To guarantee
		 * this, let linux IOMMU driver to pick up the supported capabilities
		 * when running at the bare metal if pkvm is enabled, to make it as a
		 * pkvm-awared IOMMU kernel driver.
		 *
		 * So disable SLTS and Nest.
		 */
		*ecap &= ~((1UL << 46) | (1UL << 26));
}
#endif

#endif
