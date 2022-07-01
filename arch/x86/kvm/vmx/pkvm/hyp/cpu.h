// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_CPU_H_
#define _PKVM_CPU_H_

static inline u64 pkvm_msr_read(u32 reg)
{
	u32 msrl, msrh;

	asm volatile (" rdmsr ":"=a"(msrl), "=d"(msrh) : "c" (reg));
	return (((u64)msrh << 32U) | msrl);
}

#define pkvm_rdmsr(msr, low, high)              \
do {                                            \
	u64 __val = pkvm_msr_read(msr);         \
	(void)((low) = (u32)__val);             \
	(void)((high) = (u32)(__val >> 32));    \
} while (0)

#define pkvm_rdmsrl(msr, val)                   \
	((val) = pkvm_msr_read((msr)))

static inline void pkvm_msr_write(u32 reg, u64 msr_val)
{
	asm volatile (" wrmsr " : : "c" (reg), "a" ((u32)msr_val), "d" ((u32)(msr_val >> 32U)));
}

#define pkvm_wrmsr(msr, low, high)              	\
do {                                            	\
	u64 __val = (u64)(high) << 32 | (u64)(low); 	\
	pkvm_msr_write(msr, __val);             	\
} while (0)

#define pkvm_wrmsrl(msr, val)   pkvm_msr_write(msr, val)

#ifdef CONFIG_PKVM_INTEL_DEBUG
#include <linux/smp.h>
static inline u64 get_pcpu_id(void)
{
	return raw_smp_processor_id();
}
#else
/* this function shall only be used during pkvm runtime */
static inline u64 get_pcpu_id(void)
{
	return pkvm_msr_read(MSR_GS_BASE);
}
#endif

#endif
