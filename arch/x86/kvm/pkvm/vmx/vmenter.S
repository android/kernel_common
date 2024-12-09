/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <asm/bitsperlong.h>
#include <asm/frame.h>

#define WORD_SIZE (BITS_PER_LONG / 8)

.section .noinstr.text, "ax"

#ifndef CONFIG_CC_HAS_ASM_GOTO_OUTPUT
/**
 * vmread_error_trampoline - Trampoline from inline asm to vmread_error()
 * @field:	VMCS field encoding that failed
 * @fault:	%true if the VMREAD faulted, %false if it failed
 *
 * Save and restore volatile registers across a call to vmread_error().  Note,
 * all parameters are passed on the stack.
 */
SYM_FUNC_START(vmread_error_trampoline)
	push %_ASM_BP
	mov  %_ASM_SP, %_ASM_BP

	push %_ASM_AX
	push %_ASM_CX
	push %_ASM_DX
#ifdef CONFIG_X86_64
	push %rdi
	push %rsi
	push %r8
	push %r9
	push %r10
	push %r11
#endif

	/* Load @field and @fault to arg1 and arg2 respectively. */
	mov 3*WORD_SIZE(%_ASM_BP), %_ASM_ARG2
	mov 2*WORD_SIZE(%_ASM_BP), %_ASM_ARG1

	call vmread_error_trampoline2

	/* Zero out @fault, which will be popped into the result register. */
	_ASM_MOV $0, 3*WORD_SIZE(%_ASM_BP)

#ifdef CONFIG_X86_64
	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %rsi
	pop %rdi
#endif
	pop %_ASM_DX
	pop %_ASM_CX
	pop %_ASM_AX
	pop %_ASM_BP

	RET
SYM_FUNC_END(vmread_error_trampoline)

/**
 * vmptrst_error_trampoline - Trampoline from inline asm to vmptrst_error()
 * @fault:	%true if the VMPTRST faulted, %false if it failed
 *
 * Save and restore volatile registers across a call to vmptrst_error().  Note,
 * all parameters are passed on the stack.
 */
SYM_FUNC_START(vmptrst_error_trampoline)
	push %_ASM_BP
	mov  %_ASM_SP, %_ASM_BP

	push %_ASM_AX
	push %_ASM_CX
	push %_ASM_DX
#ifdef CONFIG_X86_64
	push %rdi
	push %rsi
	push %r8
	push %r9
	push %r10
	push %r11
#endif

	/* Load @fault to arg1. */
	mov 3*WORD_SIZE(%_ASM_BP), %_ASM_ARG1

	call vmptrst_error_trampoline2

	/* Set -1 @fault, which will be popped into the result register. */
	_ASM_MOV $-1, 2*WORD_SIZE(%_ASM_BP)

#ifdef CONFIG_X86_64
	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %rsi
	pop %rdi
#endif
	pop %_ASM_DX
	pop %_ASM_CX
	pop %_ASM_AX
	pop %_ASM_BP

	RET
SYM_FUNC_END(vmptrst_error_trampoline)

#endif
