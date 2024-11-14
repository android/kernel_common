/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_VMX_H
#define __PKVM_X86_VMX_H

#include <pkvm/pkvm.h>
#include <vmx/pkvm/hyp/pkvm_hyp_types.h>

/* pkvm_vcpu_vmx is appeneded in the end of pkvm_vcpu */
/*
 * Struct pkvm_vcpu_vmx represents a vcpu structure for VMX. It requires struct
 * kvm_vcpu sitting at offset 0 so that it can be appended in the end of
 * pkvm_vcpu (see comments for pkvm_vcpu). As struct kvm_vcpu is the first field
 * of struct vcpu_vmx, vcpu_vmx would also be the first field of pkvm_vcpu_vmx.
 */
struct pkvm_vcpu_vmx {
	/* Point to the vcpu_vmx structure in pkvm */
	struct vcpu_vmx vmx;

	/*
	 * FIXME: This is to compatible with the current emulation method.
	 * Revisit this later when PV method is functional.
	 */
	struct shadow_vcpu_state shadow_vcpu;
};

/*
 * Struct pkvm_vm_vmx represents a vm structure for VMX. It requires struct kvm
 * sitting at offset 0 so that it can be appended in the end of pkvm_vm
 * (see comments for pkvm_vm). As struct kvm is the first field of struct
 * kvm_vmx, kvm_vmx would also be the first field of pkvm_vm_vmx.
 */
struct pkvm_vm_vmx {
	/* Point to the kvm_vmx structure in pkvm */
	struct kvm_vmx kvm_vmx;

	/*
	 * FIXME: This is to compatible with the emulation method. Revisit this
	 * later when PV method is functional.
	 */
	struct pkvm_shadow_vm shadow_vm;
};

static inline struct pkvm_shadow_vm *kvm_to_shadow(struct kvm *kvm)
{
	struct kvm_vmx *kvm_vmx = to_kvm_vmx(kvm);
	struct pkvm_vm_vmx *pkvm_vm_vmx;

	pkvm_vm_vmx = container_of(kvm_vmx, struct pkvm_vm_vmx, kvm_vmx);

	return &pkvm_vm_vmx->shadow_vm;
}

static inline struct shadow_vcpu_state *kvm_vcpu_to_shadow(struct kvm_vcpu *vcpu)
{
	struct pkvm_vcpu_vmx *pkvm_vcpu_vmx =
		container_of(to_vmx(vcpu), struct pkvm_vcpu_vmx, vmx);

	return &pkvm_vcpu_vmx->shadow_vcpu;
}

int setup_vmcs_config_with_setting(struct vmcs_config *vmcs_conf,
				   struct vmx_capability *vmx_cap,
				   struct vmcs_config_setting *setting);
int setup_vmx(void);

#endif /* __PKVM_X86_VMX_H */
