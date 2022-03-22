// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef _PKVM_VMEXIT_H_
#define _PKVM_VMEXIT_H_

int __pkvm_vmx_vcpu_run(unsigned long *regs, int launch);

#endif
