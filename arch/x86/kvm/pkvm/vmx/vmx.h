/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_VMX_H
#define __PKVM_X86_VMX_H

int setup_vmcs_config_with_setting(struct vmcs_config *vmcs_conf,
				   struct vmx_capability *vmx_cap,
				   struct vmcs_config_setting *setting);
int setup_vmcs_config(struct vmcs_config *vmcs_conf,
		      struct vmx_capability *vmx_cap);

#endif /* __PKVM_X86_VMX_H */
