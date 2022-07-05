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
#endif

#endif
