// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/linkage.h>
#include <asm/kvm_vcpu_regs.h>
#include <asm/frame.h>
#include <asm/asm.h>
#include <asm/bitsperlong.h>
#include <asm/unwind_hints.h>
#include <asm/nospec-branch.h>
#include <vmx/run_flags.h>
#include "kvm-asm-offsets.h"

#define WORD_SIZE (BITS_PER_LONG / 8)

#define VCPU_RAX	(__VCPU_REGS_RAX * WORD_SIZE)
#define VCPU_RCX	(__VCPU_REGS_RCX * WORD_SIZE)
#define VCPU_RDX	(__VCPU_REGS_RDX * WORD_SIZE)
#define VCPU_RBX	(__VCPU_REGS_RBX * WORD_SIZE)
#define VCPU_RBP	(__VCPU_REGS_RBP * WORD_SIZE)
#define VCPU_RSI	(__VCPU_REGS_RSI * WORD_SIZE)
#define VCPU_RDI	(__VCPU_REGS_RDI * WORD_SIZE)

#define VCPU_R8		(__VCPU_REGS_R8  * WORD_SIZE)
#define VCPU_R9		(__VCPU_REGS_R9  * WORD_SIZE)
#define VCPU_R10	(__VCPU_REGS_R10 * WORD_SIZE)
#define VCPU_R11	(__VCPU_REGS_R11 * WORD_SIZE)
#define VCPU_R12	(__VCPU_REGS_R12 * WORD_SIZE)
#define VCPU_R13	(__VCPU_REGS_R13 * WORD_SIZE)
#define VCPU_R14	(__VCPU_REGS_R14 * WORD_SIZE)
#define VCPU_R15	(__VCPU_REGS_R15 * WORD_SIZE)

/*
 * __pkvm_vmexit_entry - Handle a VM-Exit event from the host VM in pkvm.
 * The pkvm stack starts from fixed address, which is pkvm_sp_top
 * configured in VMCS.HOST_RSP.
 *
 * No return from __pkvm_vmexit_entry and vmresume to enter host VM in
 * the end, or a dead end if failed to vmresume.
 *
 * pkvm_sp_top = pcpu->stack + PKVM_STACK_SIZE - PKVM_STACK_TOP_RESV
 * pkvm_sp_top[0] --> pointer to vmx->vcpu.arch.regs
 * pkvm_sp_top[1] --> struct vcpu_vmx *vmx
 *
 */
SYM_FUNC_START(__pkvm_vmexit_entry)		// sp = pkvm_sp_top
	push %_ASM_BX				// sp = pkvm_sp_top - 8
	mov WORD_SIZE*1(%_ASM_SP), %_ASM_BX	// pointer to vmx->vcpu.arch.regs

	mov %_ASM_AX, VCPU_RAX(%_ASM_BX)
	mov %_ASM_CX, VCPU_RCX(%_ASM_BX)
	mov %_ASM_DX, VCPU_RDX(%_ASM_BX)
	mov %_ASM_SI, VCPU_RSI(%_ASM_BX)
	mov %_ASM_DI, VCPU_RDI(%_ASM_BX)
	mov %_ASM_BP, VCPU_RBP(%_ASM_BX)
	mov %r8 , VCPU_R8(%_ASM_BX)
	mov %r9 , VCPU_R9(%_ASM_BX)
	mov %r10, VCPU_R10(%_ASM_BX)
	mov %r11, VCPU_R11(%_ASM_BX)
	mov %r12, VCPU_R12(%_ASM_BX)
	mov %r13, VCPU_R13(%_ASM_BX)
	mov %r14, VCPU_R14(%_ASM_BX)
	mov %r15, VCPU_R15(%_ASM_BX)
	pop VCPU_RBX(%_ASM_BX)			//  sp = pkvm_sp_top

	mov WORD_SIZE*1(%_ASM_SP), %_ASM_DI	// RDI: point to vcpu_vmx
	mov $VMX_RUN_SAVE_SPEC_CTRL, %_ASM_SI

	call vmx_spec_ctrl_restore_host

	mov WORD_SIZE*1(%_ASM_SP), %_ASM_DI

	/* pkvm never enable interrupr */
	call pkvm_vmexit_main

	ALTERNATIVE "jmp .Lspec_ctrl_done", "", X86_FEATURE_MSR_SPEC_CTRL

	/*
	 * SPEC_CTRL handling: if the host's SPEC_CTRL value differs from the
	 * pkvm's, write the MSR.
	 *
	 * IMPORTANT: To avoid RSB underflow attacks and any other nastiness,
	 * there must not be any returns or indirect branches between this code
	 * and vmentry.
	 */
	mov WORD_SIZE*1(%_ASM_SP), %_ASM_DI
	movl VMX_spec_ctrl(%_ASM_DI), %edi
	movl PER_CPU_VAR(x86_spec_ctrl_current), %esi
	cmp %edi, %esi
	je .Lspec_ctrl_done
	mov $MSR_IA32_SPEC_CTRL, %ecx
	xor %edx, %edx
	mov %edi, %eax
	wrmsr

.Lspec_ctrl_done:

	/*
	 * Restore registers to reflect the vm-exit handling/emulation result
	 * before vmresume.
	 */
	mov VCPU_RAX(%_ASM_BX), %_ASM_AX
	mov VCPU_RCX(%_ASM_BX), %_ASM_CX
	mov VCPU_RDX(%_ASM_BX), %_ASM_DX
	mov VCPU_RSI(%_ASM_BX), %_ASM_SI
	mov VCPU_RDI(%_ASM_BX), %_ASM_DI
	mov VCPU_RBP(%_ASM_BX), %_ASM_BP
	mov VCPU_R8(%_ASM_BX),  %r8
	mov VCPU_R9(%_ASM_BX),  %r9
	mov VCPU_R10(%_ASM_BX), %r10
	mov VCPU_R11(%_ASM_BX), %r11
	mov VCPU_R12(%_ASM_BX), %r12
	mov VCPU_R13(%_ASM_BX), %r13
	mov VCPU_R14(%_ASM_BX), %r14
	mov VCPU_R15(%_ASM_BX), %r15
	mov VCPU_RBX(%_ASM_BX), %_ASM_BX

	vmresume

	// should not come here
	mov $0x4400, %_ASM_CX
	vmread %_ASM_CX, %_ASM_AX
failed:
	jmp failed			/* should never come here */
SYM_FUNC_END(__pkvm_vmexit_entry)
