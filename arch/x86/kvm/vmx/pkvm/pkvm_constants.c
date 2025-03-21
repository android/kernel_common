// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/kbuild.h>
#include <linux/bug.h>
#include <vdso/limits.h>
#include <buddy_memory.h>
#include <pkvm.h>
#include "hyp/pkvm_hyp_types.h"
#include "hyp/pkvm_iommu_types.h"
#include <pkvm/pkvm.h>
#include <pkvm/vmx/vmx.h>

int main(void)
{
	DEFINE(PKVM_VMEMMAP_ENTRY_SIZE, sizeof(struct hyp_page));
	DEFINE(PKVM_SHADOW_VM_SIZE, sizeof(struct pkvm_vm) + sizeof(struct pkvm_vm_vmx));
	DEFINE(PKVM_SHADOW_VCPU_STATE_SIZE, sizeof(struct pkvm_vcpu) +
					    sizeof(struct pkvm_vcpu_vmx));
	DEFINE(PKVM_IOMMU_NUM, PKVM_MAX_IOMMU_NUM);
	DEFINE(PKVM_PASIDDEV_NUM, PKVM_MAX_PASID_PDEV_NUM);
	DEFINE(PKVM_PDEV_NUM, PKVM_MAX_PDEV_NUM);
	DEFINE(PKVM_IOMMU_QI_DESC_SIZE, PKVM_QI_DESC_ALIGNED_SIZE);
	DEFINE(PKVM_IOMMU_QI_DESC_STATUS_SIZE, PKVM_QI_DESC_STATUS_ALIGNED_SIZE);
	DEFINE(PKVM_MAX_VM_NUM, PKVM_MAX_NORMAL_VM_NUM + PKVM_MAX_PROTECTED_VM_NUM);
	return 0;
}
