/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _ASM_X86_PKVM_H
#define _ASM_X86_PKVM_H

#include <asm/kvm_para.h>
#include <asm/io.h>
#include <asm/coco.h>

/* PKVM Hypercalls */
#define PKVM_HC_KVM_CALL		0
#define PKVM_HC_INIT_FINALISE		1
#define PKVM_HC_MMIO_ACCESS		7
#define PKVM_HC_ADD_PTDEV		10

/*
 * Internal hypercall to commit the pkvm initialization
 * status to success or failure. This is to make internal
 * hypercalls to be unavailable for general use after
 * successful pkvm initialization and to rollback pkvm
 * initialization actions on failure.
 */
#define __PKVM_HC_COMMIT_FINALISE	100

/*
 * Internal hypercall to reprivilege cpus on pkvm
 * initialization failure.
 */
#define __PKVM_HC_REPRIVILEGE_VCPU	101

/*
 * 15bits for PASID, DO NOT change it, based on it,
 * the size of PASID DIR table can kept as one page
 */
#define PKVM_MAX_PASID_BITS	15
#define PKVM_MAX_PASID		(1 << PKVM_MAX_PASID_BITS)

struct pkvm_iommu_driver {
	int (*prepare_driver)(void);
	int (*init_driver)(void);
};

enum pkvm_hc {
	__pkvm__enable_virtualization_cpu,
	__pkvm__disable_virtualization_cpu,
	__pkvm__check_processor_compatibility,
	__pkvm__vm_init,
	__pkvm__vm_finalize,
	__pkvm__vm_destroy,
	__pkvm__vm_mmu_map,
	__pkvm__vm_mmu_unmap,
	__pkvm__vm_mmu_age,
	__pkvm__vcpu_create,
	__pkvm__vcpu_free,
	__pkvm__vcpu_reset,
	__pkvm__vcpu_load,
	__pkvm__vcpu_put,
	__pkvm__vcpu_run,
	__pkvm__vcpu_after_set_cpuid,
	__pkvm__get_segment_base,
	__pkvm__get_segment,
	__pkvm__set_segment,
	__pkvm__set_cr0,
	__pkvm__set_cr4,
	__pkvm__set_msr,
	__pkvm__get_msr,
	__pkvm__set_efer,
	__pkvm__get_idt,
	__pkvm__set_idt,
	__pkvm__get_gdt,
	__pkvm__set_gdt,
	__pkvm__set_dr7,
	__pkvm__get_rflags,
	__pkvm__set_rflags,
	__pkvm__flush_tlb_all,
	__pkvm__flush_tlb_current,
	__pkvm__flush_tlb_gva,
	__pkvm__flush_tlb_guest,
	__pkvm__set_interrupt_shadow,
	__pkvm__get_interrupt_shadow,
	__pkvm__complete_emulated_msr,
	__pkvm__interrupt_allowed,
	__pkvm__nmi_allowed,
	__pkvm__inject_irq,
	__pkvm__inject_nmi,
	__pkvm__inject_exception,
	__pkvm__cancel_injection,
	__pkvm__get_nmi_mask,
	__pkvm__set_nmi_mask,
	__pkvm__enable_nmi_window,
	__pkvm__enable_irq_window,
	__pkvm__update_cr8_intercept,
	__pkvm__set_virtual_apic_mode,
	__pkvm__refresh_apicv_exec_ctrl,
	__pkvm__load_eoi_exitmap,
	__pkvm__hwapic_irr_update,
	__pkvm__hwapic_isr_update,
	__pkvm__write_tsc_offset,
	__pkvm__write_tsc_multiplier,
	__pkvm__post_set_cr3,
	__pkvm__load_mmu_pgd,
	__pkvm__setup_mce,
	__pkvm__cache_reg,
	__pkvm__update_cpuid_runtime,
	__pkvm__update_exception_bitmap,
	__pkvm__vcpu_add_fpstate,
	__pkvm__host_share_hyp,
	__pkvm__host_unshare_hyp,
};

static inline unsigned long __pkvm_hypercall(unsigned long nr, unsigned long p1,
					     unsigned long p2, unsigned long p3,
					     unsigned long p4, unsigned long p5,
					     unsigned long p6)
{
	register unsigned long r8 asm("r8") = p6;
	unsigned long ret;

	asm volatile(KVM_HYPERCALL
		     : "=a"(ret)
		     : "a"(nr), "b"(p1), "c"(p2), "d"(p3), "S"(p4), "D"(p5), "r"(r8)
		     : "memory");
	return ret;
}

#define CALL_PKVM(f)		CONCATENATE(__pkvm__, f)

#define __pkvm_hypercall_0(f)	__pkvm_hypercall(PKVM_HC_KVM_CALL, f, 0, 0, 0, 0, 0)

#define __pkvm_hypercall_1(f, a1)							\
	({										\
		__pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), 0, 0, 0, 0);				\
	})

#define __pkvm_hypercall_2(f, a1, a2)							\
	({										\
		__pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), (unsigned long)(a2), 0, 0, 0);		\
	})

#define __pkvm_hypercall_3(f, a1, a2, a3)						\
	({										\
		__pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), (unsigned long)(a2),			\
			(unsigned long)(a3), 0, 0);					\
	})

#define __pkvm_hypercall_4(f, a1, a2, a3, a4)						\
	({										\
		__pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), (unsigned long)(a2),			\
			(unsigned long)(a3), (unsigned long)(a4), 0);			\
	})

#define __pkvm_hypercall_5(f, a1, a2, a3, a4, a5)					\
	({										\
		__pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), (unsigned long)(a2),			\
			(unsigned long)(a3), (unsigned long)(a4),			\
			(unsigned long)(a5));						\
	})

#define pkvm_hypercall(f, ...)								\
	({										\
		CONCATENATE(__pkvm_hypercall_,						\
			    COUNT_ARGS(__VA_ARGS__))(CALL_PKVM(f), ##__VA_ARGS__);	\
	})

#ifdef CONFIG_PKVM_INTEL

#ifndef __PKVM_HYP__

extern bool __read_mostly enable_pkvm;	/* kernel command-line flag */

extern struct static_key_false pkvm_enabled_key;

static inline bool pkvm_enabled(void)
{
	return static_branch_likely(&pkvm_enabled_key);
}

int pkvm_iommu_register_driver(const struct pkvm_iommu_driver *kern_ops);

static inline u64 pkvm_readq(void __iomem *reg, unsigned long reg_phys,
			     unsigned long offset)
{
	if (pkvm_enabled())
		return (u64)kvm_hypercall3(PKVM_HC_MMIO_ACCESS, true,
					   sizeof(u64), reg_phys + offset);
	else
		return readq(reg + offset);
}

static inline u32 pkvm_readl(void __iomem *reg, unsigned long reg_phys,
			     unsigned long offset)
{
	if (pkvm_enabled())
		return (u32)kvm_hypercall3(PKVM_HC_MMIO_ACCESS, true,
					   sizeof(u32), reg_phys + offset);
	else
		return readl(reg + offset);
}

static inline void pkvm_writeq(void __iomem *reg, unsigned long reg_phys,
			       unsigned long offset, u64 val)
{
	if (pkvm_enabled())
		kvm_hypercall4(PKVM_HC_MMIO_ACCESS, false, sizeof(u64),
			       reg_phys + offset, val);
	else
		writeq(val, reg + offset);
}

static inline void pkvm_writel(void __iomem *reg, unsigned long reg_phys,
			       unsigned long offset, u32 val)
{
	if (pkvm_enabled())
		kvm_hypercall4(PKVM_HC_MMIO_ACCESS, false, sizeof(u32),
			       reg_phys + offset, (u64)val);
	else
		writel(val, reg + offset);
}

#else /* __PKVM_HYP__ */

/* we are in pkvm hypervisor, pkvm is enabled by definition */
#define enable_pkvm true

#endif /* __PKVM_HYP__ */

static inline void pkvm_update_iommu_virtual_caps(u64 *cap, u64 *ecap)
{
#ifndef __PKVM_HYP__
	if (!enable_pkvm)
		return;
#endif

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
	}
}
#else /* CONFIG_PKVM_INTEL */

#define enable_pkvm false

static inline bool pkvm_enabled(void)
{
	return false;
}

static inline int pkvm_iommu_register_driver(const struct pkvm_iommu_driver *kern_ops)
{
	return -EPERM;
}

#endif /* CONFIG_PKVM_INTEL */

#endif
