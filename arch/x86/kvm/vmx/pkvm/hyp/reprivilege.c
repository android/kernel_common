// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Google
 */

#include <asm/kvm_pkvm.h>

#include <pkvm.h>
#include "pkvm_hyp.h"
#include "gfp.h"
#include "debug.h"
#include "init_finalise.h"
#include <vmx/vmx.h>
#include <pkvm/vmx/vmx.h>
#include <pkvm/pkvm.h>

static inline void __repriv_restore_selectors(void)
{
	static u16 guest_ds, guest_es, guest_fs, guest_gs, guest_ss;
	static u64 guest_fsbase, guest_gsbase;

	guest_fsbase = vmcs_readl(GUEST_FS_BASE);
	guest_gsbase = vmcs_readl(GUEST_GS_BASE);

	guest_ds = vmcs_read16(GUEST_DS_SELECTOR);
	guest_es = vmcs_read16(GUEST_ES_SELECTOR);
	guest_fs = vmcs_read16(GUEST_FS_SELECTOR);
	guest_gs = vmcs_read16(GUEST_GS_SELECTOR);
	guest_ss = vmcs_read16(GUEST_SS_SELECTOR);

	asm volatile (
		"mov %0, %%ds\n"
		"mov %1, %%es\n"
		"mov %2, %%fs\n"
		"mov %3, %%gs\n"
		"mov %4, %%ss\n"

		:
		: "m"(guest_ds), "m"(guest_es),
		  "m"(guest_fs), "m"(guest_gs), "m"(guest_ss)
		: "memory"
	);

	wrmsrl(MSR_FS_BASE, guest_fsbase);
	wrmsrl(MSR_GS_BASE, guest_gsbase);
}

#define PKVM_WRITE_CR(crnum, val) \
static inline void __pkvm_write_cr##crnum(unsigned long val) \
{							\
	asm volatile("mov %0,%%cr" #crnum : "+r" (val) : : "memory"); \
}

PKVM_WRITE_CR(0, val)
PKVM_WRITE_CR(3, val)
PKVM_WRITE_CR(4, val)

static void __repriv_restore_special_regs(u64 guest_cr0, u64 guest_cr3, u64 guest_cr4,
		struct desc_ptr *gdt, struct desc_ptr *idt)
{
	struct desc_struct *guest_gdt;
	tss_desc *tss;

	asm volatile (
		"lgdt %0\n"
		"lidt %1\n"

		:
		: "m"(*gdt), "m"(*idt)
		: "memory"
	);

	/*
	 * Reset the busy bit to reload TR
	 */
	guest_gdt = (struct desc_struct *)(gdt->address);
	tss = (tss_desc *)&guest_gdt[GDT_ENTRY_TSS];
	tss->type = DESC_TSS;
	asm volatile("ltr %w0"::"q" (GDT_ENTRY_TSS*8));

	__pkvm_write_cr0(guest_cr0);
	__pkvm_write_cr4(guest_cr4);
	__pkvm_write_cr3(guest_cr3);
}

#define WORD_SIZE (BITS_PER_LONG / 8)

#define VCPU_RAX	(__VCPU_REGS_RAX * WORD_SIZE)
#define VCPU_RCX	(__VCPU_REGS_RCX * WORD_SIZE)
#define VCPU_RDX	(__VCPU_REGS_RDX * WORD_SIZE)
#define VCPU_RBX	(__VCPU_REGS_RBX * WORD_SIZE)
#define VCPU_RBP	(__VCPU_REGS_RBP * WORD_SIZE)
#define VCPU_RSI	(__VCPU_REGS_RSI * WORD_SIZE)
#define VCPU_RDI	(__VCPU_REGS_RDI * WORD_SIZE)

#ifdef CONFIG_X86_64
#define VCPU_R8		(__VCPU_REGS_R8  * WORD_SIZE)
#define VCPU_R9		(__VCPU_REGS_R9  * WORD_SIZE)
#define VCPU_R10	(__VCPU_REGS_R10 * WORD_SIZE)
#define VCPU_R11	(__VCPU_REGS_R11 * WORD_SIZE)
#define VCPU_R12	(__VCPU_REGS_R12 * WORD_SIZE)
#define VCPU_R13	(__VCPU_REGS_R13 * WORD_SIZE)
#define VCPU_R14	(__VCPU_REGS_R14 * WORD_SIZE)
#define VCPU_R15	(__VCPU_REGS_R15 * WORD_SIZE)
#endif

#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

/*
 * Restores register state from memory pointed by rdi
 * offset: offset of register backup in memory
 * dest_reg: register to be restored.
 */
#define RESTORE_VCPU_REG(offset, dest_reg) \
	"mov " STRINGIFY(offset) "(%%rdi), %%" #dest_reg "\n"

/*
 * Restores host cpu state and returns to host with vmx off
 */
void pkvm_repriv_restore_cpu(unsigned long *vcpu_regs)
{
	/*
	 * We manipulate SP in assembly. So don't use
	 * stack for the variables.
	 */
	static u64 guest_rip, guest_rsp, guest_rflags;
	static u64 guest_cr0, guest_cr3, guest_cr4;
	static struct desc_ptr gdt, idt;
	static u16 guest_cs, guest_ss;

	native_irq_disable();

	gdt.address = vmcs_readl(GUEST_GDTR_BASE);
	gdt.size = vmcs_read32(GUEST_GDTR_LIMIT);

	idt.address = vmcs_readl(GUEST_IDTR_BASE);
	idt.size = vmcs_read32(GUEST_IDTR_LIMIT);

	guest_rsp = vmcs_readl(GUEST_RSP);
	guest_ss = vmcs_read16(GUEST_SS_SELECTOR);
	guest_cs = vmcs_read16(GUEST_CS_SELECTOR);
	guest_cr0 = vmcs_readl(GUEST_CR0);
	guest_cr3 = vmcs_readl(GUEST_CR3);
	guest_cr4 = vmcs_readl(GUEST_CR4);
	guest_rip = vmcs_readl(GUEST_RIP) + vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	guest_rflags = vmcs_readl(GUEST_RFLAGS);

	__repriv_restore_selectors();

	asm volatile ("vmxoff" ::: "memory");

	__repriv_restore_special_regs(guest_cr0, guest_cr3, guest_cr4, &gdt, &idt);

	asm volatile(
		/* Restore general purpose registers */
		RESTORE_VCPU_REG(VCPU_RAX, rax)
		RESTORE_VCPU_REG(VCPU_RCX, rcx)
		RESTORE_VCPU_REG(VCPU_RDX, rdx)
		RESTORE_VCPU_REG(VCPU_RBX, rbx)
		RESTORE_VCPU_REG(VCPU_RBP, rbp)
		RESTORE_VCPU_REG(VCPU_RSI, rsi)

#ifdef CONFIG_X86_64
		RESTORE_VCPU_REG(VCPU_R8, r8)
		RESTORE_VCPU_REG(VCPU_R9, r9)
		RESTORE_VCPU_REG(VCPU_R10, r10)
		RESTORE_VCPU_REG(VCPU_R11, r11)
		RESTORE_VCPU_REG(VCPU_R12, r12)
		RESTORE_VCPU_REG(VCPU_R13, r13)
		RESTORE_VCPU_REG(VCPU_R14, r14)
		RESTORE_VCPU_REG(VCPU_R15, r15)
#endif

		/* Restore RDI (last!) */
		RESTORE_VCPU_REG(VCPU_RDI, rdi)

		/*
		 * update stack as expected by iretq
		 */
		"pushq %0\n"
		"pushq %1\n"
		"pushq %2\n"
		"pushq %3\n"
		"pushq %4\n"

		"iretq\n"

		:
		: "m"(guest_ss), "m"(guest_rsp), "m"(guest_rflags),
		  "m"(guest_cs), "m"(guest_rip), "D"(vcpu_regs)
		: "memory", "cc"
	);
}
STACK_FRAME_NON_STANDARD(pkvm_repriv_restore_cpu);
