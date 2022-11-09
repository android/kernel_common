// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_VMX_OPS_H_
#define _PKVM_VMX_OPS_H_

#include "debug.h"

static __always_inline unsigned long __vmcs_readl(unsigned long field)
{
	unsigned long value;

#ifdef CONFIG_CC_HAS_ASM_GOTO_OUTPUT
	asm_goto_output("1: vmread %[field], %[output]\n\t"
			  "jna %l[do_fail]\n\t"
			  : [output] "=r" (value)
			  : [field] "r" (field)
			  : "cc"
			  : do_fail);

	return value;

do_fail:
	pkvm_err("pkvm: vmread failed: field=%lx\n", field);
	return 0;
#else
	asm volatile ("vmread %%rdx, %%rax "
			: "=a" (value)
			: "d"(field)
			: "cc");
	return value;
#endif
}

static __always_inline u16 vmcs_read16(unsigned long field)
{
	vmcs_check16(field);
	return __vmcs_readl(field);
}

static __always_inline u32 vmcs_read32(unsigned long field)
{
	vmcs_check32(field);
	return __vmcs_readl(field);
}

static __always_inline u64 vmcs_read64(unsigned long field)
{
	vmcs_check64(field);
	return __vmcs_readl(field);
}

static __always_inline unsigned long vmcs_readl(unsigned long field)
{
	vmcs_checkl(field);
	return __vmcs_readl(field);
}

static inline void pkvm_vmwrite_error(unsigned long field, unsigned long value)
{
	pkvm_err("pkvm: vmwrite failed: field=%lx val=%lx err=%d\n",
			field, value, vmcs_read32(VM_INSTRUCTION_ERROR));
}

static inline void pkvm_vmclear_error(struct vmcs *vmcs, u64 phys_addr)
{
	pkvm_err("pkvm: vmclear failed: %p/%llx\n", vmcs, phys_addr);
}

static inline void pkvm_vmptrld_error(struct vmcs *vmcs, u64 phys_addr)
{
	pkvm_err("pkvm: vmptrld failed: %p/%llx\n", vmcs, phys_addr);
}

static inline void pkvm_invvpid_error(unsigned long ext, u16 vpid, gva_t gva)
{
	pkvm_err("pkvm: invvpid failed: ext=0x%lx vpid=%u gva=0x%lx\n",
			ext, vpid, gva);
}

static inline void pkvm_invept_error(unsigned long ext, u64 eptp, gpa_t gpa)
{
	pkvm_err("pkvm: invept failed: ext=0x%lx eptp=%llx gpa=0x%llx\n",
			ext, eptp, gpa);
}

#define vmx_asm1(insn, op1, error_args...)				\
do {									\
	asm goto(__stringify(insn) " %0\n\t"			\
			  ".byte 0x2e\n\t" /* branch not taken hint */	\
			  "jna %l[error]\n\t"				\
			  : : op1 : "cc" : error);			\
	return;								\
error:									\
	pkvm_##insn##_error(error_args);					\
	return;								\
} while (0)

#define vmx_asm2(insn, op1, op2, error_args...)				\
do {									\
	asm goto(__stringify(insn) " %1, %0\n\t"		\
			  ".byte 0x2e\n\t" /* branch not taken hint */	\
			  "jna %l[error]\n\t"				\
			  : : op1, op2 : "cc" : error);			\
	return;								\
error:									\
	pkvm_##insn##_error(error_args);					\
	return;								\
} while (0)

static __always_inline void __vmcs_writel(unsigned long field, unsigned long value)
{
	vmx_asm2(vmwrite, "r"(field), "rm"(value), field, value);
}

static __always_inline void vmcs_write16(unsigned long field, u16 value)
{
	vmcs_check16(field);
	__vmcs_writel(field, value);
}

static __always_inline void vmcs_write32(unsigned long field, u32 value)
{
	vmcs_check32(field);
	__vmcs_writel(field, value);
}

static __always_inline void vmcs_write64(unsigned long field, u64 value)
{
	vmcs_check64(field);
	__vmcs_writel(field, value);
}

static __always_inline void vmcs_writel(unsigned long field, unsigned long value)
{
	vmcs_checkl(field);
	__vmcs_writel(field, value);
}

static __always_inline void vmcs_clear_bits(unsigned long field, u32 mask)
{
	BUILD_BUG_ON_MSG(__builtin_constant_p(field) && ((field) & 0x6000) == 0x2000,
			 "vmcs_clear_bits does not support 64-bit fields");
	__vmcs_writel(field, __vmcs_readl(field) & ~mask);
}

static __always_inline void vmcs_set_bits(unsigned long field, u32 mask)
{
	BUILD_BUG_ON_MSG(__builtin_constant_p(field) && ((field) & 0x6000) == 0x2000,
			 "vmcs_set_bits does not support 64-bit fields");
	__vmcs_writel(field, __vmcs_readl(field) | mask);
}

static inline void vmcs_clear(struct vmcs *vmcs)
{
	u64 phys_addr = __pa(vmcs);

	vmx_asm1(vmclear, "m"(phys_addr), vmcs, phys_addr);
}

static inline void vmcs_load(struct vmcs *vmcs)
{
	u64 phys_addr = __pa(vmcs);

	vmx_asm1(vmptrld, "m"(phys_addr), vmcs, phys_addr);
}

#endif
