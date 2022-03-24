// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_MEMORY_H_
#define _PKVM_MEMORY_H_

#include <asm/kvm_pkvm.h>

unsigned long pkvm_virt_to_symbol_phys(void *virt);
#define __pkvm_pa_symbol(x) pkvm_virt_to_symbol_phys((void *)x)

#endif
