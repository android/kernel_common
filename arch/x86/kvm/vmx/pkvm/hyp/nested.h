// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_NESTED_H
#define __PKVM_NESTED_H

int handle_vmxon(struct kvm_vcpu *vcpu);
int handle_vmxoff(struct kvm_vcpu *vcpu);
int handle_vmptrld(struct kvm_vcpu *vcpu);
int handle_vmclear(struct kvm_vcpu *vcpu);
int handle_vmwrite(struct kvm_vcpu *vcpu);
int handle_vmread(struct kvm_vcpu *vcpu);
void pkvm_init_nest(void);

#endif
