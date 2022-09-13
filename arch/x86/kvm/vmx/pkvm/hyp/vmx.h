// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef __PKVM_VMX_H
#define __PKVM_VMX_H

#include "pkvm_hyp.h"

static inline u64 pkvm_construct_eptp(unsigned long root_hpa, int level)
{
	u64 eptp = 0;

	if ((level == 4) && vmx_ept_has_4levels())
		eptp = VMX_EPTP_PWL_4;
	else if ((level == 5) && vmx_ept_has_5levels())
		eptp = VMX_EPTP_PWL_5;

	if (vmx_ept_has_mt_wb())
		eptp |= VMX_EPTP_MT_WB;

	eptp |= (root_hpa & PAGE_MASK);

	return eptp;
}

static inline void vmx_enable_irq_window(struct vcpu_vmx *vmx)
{
	exec_controls_setbit(vmx, CPU_BASED_INTR_WINDOW_EXITING);
}

static inline void vmcs_load_track(struct vcpu_vmx *vmx, struct vmcs *vmcs)
{
	struct pkvm_host_vcpu *pkvm_host_vcpu = vmx_to_pkvm_hvcpu(vmx);

	pkvm_host_vcpu->current_vmcs = vmcs;
	barrier();
	vmcs_load(vmcs);
}

static inline void vmcs_clear_track(struct vcpu_vmx *vmx, struct vmcs *vmcs)
{
	struct pkvm_host_vcpu *pkvm_host_vcpu = vmx_to_pkvm_hvcpu(vmx);

	/* vmcs_clear might clear non-current vmcs */
	if (pkvm_host_vcpu->current_vmcs == vmcs)
		pkvm_host_vcpu->current_vmcs = NULL;

	barrier();
	vmcs_clear(vmcs);
}

static inline void flush_ept(u64 eptp)
{
	if (vmx_has_invept_context())
		__invept(VMX_EPT_EXTENT_CONTEXT, eptp, 0);
	else
		__invept(VMX_EPT_EXTENT_GLOBAL, 0, 0);
}

static inline u8 pkvm_virt_addr_bits(void)
{
	return (vmcs_readl(GUEST_CR4) & X86_CR4_LA57) ? 57 : 48;
}

void pkvm_init_host_state_area(struct pkvm_pcpu *pcpu, int cpu);

#endif
