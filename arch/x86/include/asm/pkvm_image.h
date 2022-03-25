/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef __X86_INTEL_PKVM_IMAGE_H
#define __X86_INTEL_PKVM_IMAGE_H

#if defined(CONFIG_PKVM_INTEL_DEBUG) || defined(__PKVM_HYP__)
/* No prefix will be added */
#define PKVM_DECLARE(type, f)	type f
#define pkvm_sym(sym)		sym
#else
/* prefix is added by Makefile */
#define PKVM_DECLARE(type, f)	type __pkvm_##f
#define pkvm_sym(sym)		__pkvm_##sym
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

#endif /* __X86_INTEL_PKVM_IMAGE_H */
