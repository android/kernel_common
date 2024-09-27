/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2002 Andi Kleen, SuSE Labs */

#include <linux/linkage.h>

/*
 * ISO C memset - set a memory block to a byte value. This function uses
 * enhanced rep stosb to override the fast string function.
 * The code is simpler and shorter than the fast string function as well.
 *
 * rdi   destination
 * rsi   value (char)
 * rdx   count (bytes)
 *
 * rax   original destination
 */
SYM_FUNC_START(memset)
	movq %rdi,%r9
	movb %sil,%al
	movq %rdx,%rcx
	rep stosb
	movq %r9,%rax
	RET
SYM_FUNC_END(memset)
