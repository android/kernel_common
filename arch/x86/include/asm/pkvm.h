/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _ASM_X86_PKVM_H
#define _ASM_X86_PKVM_H

#include <asm/kvm_para.h>
#include <asm/io.h>
#include <asm/coco.h>
#include <asm/virt_exception.h>

/* PKVM Hypercalls */
#define PKVM_HC_INIT_FINALISE		1
#define PKVM_HC_INIT_SHADOW_VM		2
#define PKVM_HC_INIT_SHADOW_VCPU	3
#define PKVM_HC_TEARDOWN_SHADOW_VM	4
#define PKVM_HC_TEARDOWN_SHADOW_VCPU	5
#define PKVM_HC_MMIO_ACCESS		6
#define PKVM_HC_ACTIVATE_IOMMU		7
#define PKVM_HC_TLB_REMOTE_FLUSH_RANGE	8
#define PKVM_HC_SET_MMIO_VE		9
#define PKVM_HC_ADD_PTDEV		10

/*
 * 15bits for PASID, DO NOT change it, based on it,
 * the size of PASID DIR table can kept as one page
 */
#define PKVM_MAX_PASID_BITS	15
#define PKVM_MAX_PASID		(1 << PKVM_MAX_PASID_BITS)

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

	if (ecap) {
		u64 tmp;

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

		/* limit PASID to reduce the memory consumptions */
		tmp = min_t(u64, (PKVM_MAX_PASID_BITS - 1),
			    (*ecap & GENMASK_ULL(39, 35)) >> 35);
		*ecap = (*ecap & ~GENMASK_ULL(39, 35)) | (tmp << 35);

		/*
		 * Disable Device TLB capability for security.
		 *
		 * ATS is only enabled for trusted device by the host OS.
		 * However with pkvm, the host OS including the device driver
		 * is treated as untrusted software. A malicious software in
		 * host OS may enable ATS for untrusted device so that the
		 * untrusted device can still exploit the ATS weekness to bypass
		 * VT-d's translation protection and access the isolated memory.
		 *
		 * To resolve this, tell the host IOMMU driver not to enable
		 * any device's ATS as pkvm controls IOMMU not to enable the
		 * device TLB.
		 */
		*ecap &= ~(1UL << 2);
	}
}
#endif

#ifdef CONFIG_PKVM_GUEST

void pkvm_guest_init_coco(void);
bool pkvm_is_protected_guest(void);
int pkvm_set_mem_host_visibility(unsigned long addr, int numpages, bool enc);

u64 __pkvm_module_call(u64 fn, struct tdx_module_args *out);

#else

static inline void pkvm_guest_init_coco(void) { }
static inline bool pkvm_is_protected_guest(void) { return false; }
static inline int
pkvm_set_mem_host_visibility(unsigned long addr, int numpages, bool enc) { return 0; }

#endif

#endif
