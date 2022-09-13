/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef _PKVM_CAPS_H_
#define _PKVM_CAPS_H_

#ifdef __PKVM_HYP__
#define PKVM_HYP pkvm_hyp
#else
#define PKVM_HYP pkvm_sym(pkvm_hyp)
#endif

static inline bool vmx_has_vmwrite_any_field(void)
{
	return !!(PKVM_HYP->vmcs_config.nested.misc_low &
			VMX_MISC_VMWRITE_SHADOW_RO_FIELDS);
}

static inline bool vmx_ept_capability_check(u32 bit)
{
	struct vmx_capability *vmx_cap = &PKVM_HYP->vmx_cap;

	return vmx_cap->ept & bit;
}

static inline bool vmx_has_invept(void)
{
	return vmx_ept_capability_check(VMX_EPT_INVEPT_BIT);
}

static inline bool vmx_has_ept_execute_only(void)
{
	return vmx_ept_capability_check(VMX_EPT_EXECUTE_ONLY_BIT);
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

static inline bool vmx_has_invept_context(void)
{
	return vmx_ept_capability_check(VMX_EPT_EXTENT_CONTEXT_BIT);
}

static inline bool vmx_vpid_capability_check(u32 bit)
{
	struct vmx_capability *vmx_cap = &PKVM_HYP->vmx_cap;

	return vmx_cap->vpid & bit;
}

static inline bool vmx_has_invvpid(void)
{
	return vmx_vpid_capability_check(VMX_VPID_INVVPID_BIT);
}

static inline bool vmx_has_invvpid_individual_addr(void)
{
	return vmx_vpid_capability_check(VMX_VPID_EXTENT_INDIVIDUAL_ADDR_BIT);
}

static inline bool vmx_has_invvpid_single(void)
{
	return vmx_vpid_capability_check(VMX_VPID_EXTENT_SINGLE_CONTEXT_BIT);
}

static inline bool vmx_has_invvpid_global(void)
{
	return vmx_vpid_capability_check(VMX_VPID_EXTENT_GLOBAL_CONTEXT_BIT);
}

#endif
