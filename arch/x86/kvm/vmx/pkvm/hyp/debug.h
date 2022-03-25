// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef _PKVM_DEBUG_H_
#define _PKVM_DEBUG_H_

#ifdef CONFIG_PKVM_INTEL_DEBUG
#include <linux/printk.h>
#define pkvm_dbg(f, x...) pr_debug(f, ## x)
#define pkvm_info(f, x...) pr_info(f, ## x)
#define pkvm_err(f, x...) pr_err(f, ## x)
#else
#define pkvm_dbg(x...)
#define pkvm_info(x...)
#define pkvm_err(x...)
#endif

#endif
