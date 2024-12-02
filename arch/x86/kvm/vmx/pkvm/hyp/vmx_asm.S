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

#define HOST_RSP	0x6C14

/**
 * __vmenter - VM-Enter the current loaded VMCS
 *
 * Returns:
 *	%RFLAGS.CF is set on VM-Fail Invalid
 *	%RFLAGS.ZF is set on VM-Fail Valid
 *	%RFLAGS.{CF,ZF} are cleared on VM-Success, i.e. VM-Exit
 *
 * Note that VMRESUME/VMLAUNCH fall-through and return directly if
 * they VM-Fail, whereas a successful VM-Enter + VM-Exit will jump
 * to vmx_vmexit.
 */
SYM_FUNC_START_LOCAL(__vmenter)
	/* EFLAGS.ZF is set if VMCS.LAUNCHED == 0 */
	je 2f

1:	vmresume
	ANNOTATE_UNRET_SAFE
	ret

2:	vmlaunch
	ANNOTATE_UNRET_SAFE
	ret
SYM_FUNC_END(__vmenter)

/**
 * __pkvm_vmx_vmexit - Handle a VMX VM-Exit
 *
 * Returns:
 *	%RFLAGS.{CF,ZF} are cleared on VM-Success, i.e. VM-Exit
 *
 * This is __vmenter's partner in crime.  On a VM-Exit, control will jump
 * here after hardware loads the host's state, i.e. this is the destination
 * referred to by VMCS.HOST_RIP.
 */
SYM_FUNC_START(__pkvm_vmx_vmexit)
	ANNOTATE_UNRET_SAFE
	ret
SYM_FUNC_END(__pkvm_vmx_vmexit)

/**
 * __pkvm_vmx_vcpu_run - Run a vCPU via a transition to VMX guest mode
 * @regs:	unsigned long * (to guest registers)
 * @launched:	%true if the VMCS has been launched
 *
 * Returns:
 *	0 on VM-Exit, 1 on VM-Fail
 */
SYM_FUNC_START(__pkvm_vmx_vcpu_run)
	push %_ASM_BP
	mov  %_ASM_SP, %_ASM_BP
	push %r15
	push %r14
	push %r13
	push %r12

	push %_ASM_BX

	push %_ASM_ARG1

	/* record host RSP (0x6C14) */
	mov $HOST_RSP, %_ASM_BX
	lea -WORD_SIZE(%_ASM_SP), %_ASM_CX
	vmwrite %_ASM_CX, %_ASM_BX

	mov %_ASM_ARG1, %_ASM_CX
	cmp $1, %_ASM_ARG2

	mov VCPU_RAX(%_ASM_CX), %_ASM_AX
	mov VCPU_RBX(%_ASM_CX), %_ASM_BX
	mov VCPU_RDX(%_ASM_CX), %_ASM_DX
	mov VCPU_RSI(%_ASM_CX), %_ASM_SI
	mov VCPU_RDI(%_ASM_CX), %_ASM_DI
	mov VCPU_RBP(%_ASM_CX), %_ASM_BP
	mov VCPU_R8(%_ASM_CX),  %r8
	mov VCPU_R9(%_ASM_CX),  %r9
	mov VCPU_R10(%_ASM_CX), %r10
	mov VCPU_R11(%_ASM_CX), %r11
	mov VCPU_R12(%_ASM_CX), %r12
	mov VCPU_R13(%_ASM_CX), %r13
	mov VCPU_R14(%_ASM_CX), %r14
	mov VCPU_R15(%_ASM_CX), %r15

	mov VCPU_RCX(%_ASM_CX), %_ASM_CX

	call __vmenter

	/* Jump on VM-Fail. */
	jbe 2f

	push %_ASM_CX
	mov WORD_SIZE(%_ASM_SP), %_ASM_CX

	mov %_ASM_AX, VCPU_RAX(%_ASM_CX)
	mov %_ASM_BX, VCPU_RBX(%_ASM_CX)
	mov %_ASM_DX, VCPU_RDX(%_ASM_CX)
	mov %_ASM_SI, VCPU_RSI(%_ASM_CX)
	mov %_ASM_DI, VCPU_RDI(%_ASM_CX)
	mov %_ASM_BP, VCPU_RBP(%_ASM_CX)
	mov %r8 , VCPU_R8(%_ASM_CX)
	mov %r9 , VCPU_R9(%_ASM_CX)
	mov %r10, VCPU_R10(%_ASM_CX)
	mov %r11, VCPU_R11(%_ASM_CX)
	mov %r12, VCPU_R12(%_ASM_CX)
	mov %r13, VCPU_R13(%_ASM_CX)
	mov %r14, VCPU_R14(%_ASM_CX)
	mov %r15, VCPU_R15(%_ASM_CX)

	pop VCPU_RCX(%_ASM_CX)

	/* Clear RAX to indicate VM-Exit (as opposed to VM-Fail). */
	xor %eax, %eax

	/*
	 * Clear all general purpose registers except RSP and RAX to prevent
	 * speculative use of the guest's values, even those that are reloaded
	 * via the stack.  In theory, an L1 cache miss when restoring registers
	 * could lead to speculative execution with the guest's values.
	 * Zeroing XORs are dirt cheap, i.e. the extra paranoia is essentially
	 * free.  RSP and RAX are exempt as RSP is restored by hardware during
	 * VM-Exit and RAX is explicitly loaded with 0 or 1 to return VM-Fail.
	 */
1:	xor %ebx, %ebx
	xor %ecx, %ecx
	xor %edx, %edx
	xor %esi, %esi
	xor %edi, %edi
	xor %ebp, %ebp
	xor %r8d,  %r8d
	xor %r9d,  %r9d
	xor %r10d, %r10d
	xor %r11d, %r11d
	xor %r12d, %r12d
	xor %r13d, %r13d
	xor %r14d, %r14d
	xor %r15d, %r15d

	/* "POP" @regs. */
	add $WORD_SIZE, %_ASM_SP
	pop %_ASM_BX

	pop %r12
	pop %r13
	pop %r14
	pop %r15

	pop %_ASM_BP
	ANNOTATE_UNRET_SAFE
	ret
	/* VM-Fail.  Out-of-line to avoid a taken Jcc after VM-Exit. */
2:	mov $1, %eax
	jmp 1b
SYM_FUNC_END(__pkvm_vmx_vcpu_run)

/*
 * __pkvm_vmexit_entry - Handle a VM-Exit event in pkvm.
 * The pkvm stack starts from fixed address, which is pkvm_sp_top
 * configured in VMCS.HOST_RSP.
 * Return:
 *	true to use vmlaunch instruction.
 *
 * pkvm_sp_top = pcpu->stack + PKVM_STACK_SIZE - PKVM_STACK_TOP_RESV
 * pkvm_sp_top[0] --> pointer to vcpu->arch.regs
 * pkvm_sp_top[1] --> struct kvm_vcpu *vcpu
 *
 */
SYM_FUNC_START(__pkvm_vmexit_entry)		// sp = pkvm_sp_top
	push %_ASM_BX				// sp = pkvm_sp_top - 8
	mov WORD_SIZE*1(%_ASM_SP), %_ASM_BX	// pointer to vcpu->arch.regs

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

	mov WORD_SIZE*1(%_ASM_SP), %_ASM_DI	// RDI: point to kvm_vcpu

	/* pkvm never enable interrupr */
	call pkvm_vmexit_main
	cmp $1, %_ASM_AX			// use vmlaunch ?

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

	jne 1f
	vmlaunch
	jmp 2f
1:
	vmresume
2:
	// should not come here
	mov $0x4400, %_ASM_CX
	vmread %_ASM_CX, %_ASM_AX
failed:
	jmp failed			/* should never come here */
SYM_FUNC_END(__pkvm_vmexit_entry)
