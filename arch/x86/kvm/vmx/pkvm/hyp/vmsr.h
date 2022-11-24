// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_VMSR_H_
#define _PKVM_VMSR_H_

int handle_read_msr(struct kvm_vcpu *vcpu);
int handle_write_msr(struct kvm_vcpu *vcpu);

#endif
