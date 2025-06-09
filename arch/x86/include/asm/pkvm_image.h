/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef __X86_INTEL_PKVM_IMAGE_H
#define __X86_INTEL_PKVM_IMAGE_H

#include <linux/types.h>

#if defined(__PKVM_HYP__)
/* No suffix will be added */
#define PKVM_DECLARE(type, f, params)	type f params
#define pkvm_sym(sym)		sym
#else
/* suffix is added by Makefile */
#define PKVM_DECLARE(type, f, params)	type f##__pkvm params
#define pkvm_sym(sym)		sym##__pkvm
#endif

#define __PKVM_CONCAT(a, b)	a ## b
#define PKVM_CONCAT(a, b)	__PKVM_CONCAT(a, b)

#ifdef LINKER_SCRIPT

#define PKVM_SECTION_NAME(NAME)	.pkvm##NAME

#define PKVM_SECTION_SYMBOL_NAME(NAME) \
	PKVM_CONCAT(__pkvm_section_, PKVM_SECTION_NAME(NAME))

#define BEGIN_PKVM_SECTION(NAME)			\
	PKVM_SECTION_NAME(NAME) : {			\
		PKVM_SECTION_SYMBOL_NAME(NAME) = .;

#define END_PKVM_SECTION				\
	}

#define PKVM_SECTION(NAME)			\
	BEGIN_PKVM_SECTION(NAME)		\
		*(NAME NAME##.*)		\
	END_PKVM_SECTION

/*
 * Defines a linker script alias of a kernel-proper symbol referenced by
 * PKVM code.
 */
#define PKVM_ALIAS(sym)  pkvm_sym(sym) = sym;

#endif /* LINKER_SCRIPT */

#ifndef __ASSEMBLER__

#ifdef CONFIG_PKVM_INTEL
extern char __pkvm_text_start[], __pkvm_text_end[];
extern char __pkvm_rodata_start[], __pkvm_rodata_end[];
extern char __pkvm_data_start[], __pkvm_data_end[];
extern char __pkvm_bss_start[], __pkvm_bss_end[];
static inline bool is_pkvm_text(void *addr)
{
	return addr >= (void *)__pkvm_text_start && addr < (void *)__pkvm_text_end;
}
#else
static inline bool is_pkvm_text(void *addr) { return false; }
#endif

#endif

#endif /* __X86_INTEL_PKVM_IMAGE_H */
