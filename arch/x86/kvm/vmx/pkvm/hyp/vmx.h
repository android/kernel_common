// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef __PKVM_VMX_H
#define __PKVM_VMX_H

static inline bool vmx_ept_capability_check(u32 bit)
{
	struct vmx_capability *vmx_cap = &pkvm_hyp->vmx_cap;

	return vmx_cap->ept & bit;
}

static inline bool vmx_ept_has_4levels(void)
{
	return vmx_ept_capability_check(VMX_EPT_PAGE_WALK_4_BIT);
}

static inline bool vmx_ept_has_5levels(void)
{
	return vmx_ept_capability_check(VMX_EPT_PAGE_WALK_5_BIT);
}

static inline bool vmx_ept_has_mt_wb(void)
{
	return vmx_ept_capability_check(VMX_EPTP_WB_BIT);
}

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

void pkvm_init_host_state_area(struct pkvm_pcpu *pcpu, int cpu);

#endif
