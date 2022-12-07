// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/kbuild.h>
#include <linux/bug.h>
#include <vdso/limits.h>
#include <buddy_memory.h>
#include <vmx/vmx.h>
#include "hyp/pkvm_hyp.h"

int main(void)
{
	DEFINE(PKVM_VMEMMAP_ENTRY_SIZE, sizeof(struct hyp_page));
	DEFINE(PKVM_SHADOW_VM_SIZE, sizeof(struct pkvm_shadow_vm) + pkvm_shadow_vcpu_array_size());
	DEFINE(PKVM_SHADOW_VCPU_STATE_SIZE, sizeof(struct shadow_vcpu_state));
	return 0;
}
