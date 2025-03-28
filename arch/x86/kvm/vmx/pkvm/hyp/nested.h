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
int handle_vmresume(struct kvm_vcpu *vcpu);
int handle_vmlaunch(struct kvm_vcpu *vcpu);
int handle_invept(struct kvm_vcpu *vcpu);
int handle_invvpid(struct kvm_vcpu *vcpu);
int nested_vmexit(struct kvm_vcpu *vcpu, bool *skip_instruction);
void nested_flush_shadow_ept(struct kvm_vcpu *vcpu);
void nested_invalidate_shadow_ept(int shadow_handle, u64 start_gpa, u64 size);
void pkvm_init_nest(void);

#define LIST_OF_VMX_MSRS        		\
	MSR_IA32_VMX_MISC,                      \
	MSR_IA32_VMX_PROCBASED_CTLS2,           \
	MSR_IA32_VMX_EPT_VPID_CAP,              \
	MSR_IA32_VMX_VMFUNC

bool is_vmx_msr(unsigned long msr);
int read_vmx_msr(struct kvm_vcpu *vcpu, unsigned long msr, u64 *val);

#endif
