// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_BUG_H
#define __PKVM_BUG_H

#ifdef CONFIG_PKVM_INTEL_DEBUG
#include <linux/printk.h>

#define PKVM_ASSERT(c)                                          \
do {                                                            \
	if (!(c)) {                                             \
		pr_err("assertion failed %s: %d: %s\n",         \
			__FILE__, __LINE__, #c);                \
		BUG();                                          \
	}                                                       \
} while (0)
#else
#define PKVM_ASSERT(c) do { } while (!(c))
#endif

#endif
