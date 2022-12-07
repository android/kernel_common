/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/asm-offsets.h>
#include <asm/asm.h>
#include <asm/frame.h>
#include <asm/unwind_hints.h>

#include <linux/linkage.h>
#include <linux/bits.h>
#include <linux/errno.h>

.macro PKVM_MODULE_CALL
	/* Save the output parameter. */
	push %r12

	/* Push output pointer to stack. */
	push %rsi

	mov %rdi, %rax

	vmcall

	pop %r12

	test %r12, %r12
	jz .Lno_output_struct

	/* Copy result registers to output struct. */
	movq %rcx, 0(%r12)
	movq %rdx, 8(%r12)
	movq %r8, 16(%r12)
	movq %r9, 24(%r12)

.Lno_output_struct:
	pop %r12
.endm

SYM_FUNC_START(__pkvm_module_call)
	FRAME_BEGIN
	PKVM_MODULE_CALL
	FRAME_END
	RET
SYM_FUNC_END(__pkvm_module_call)
