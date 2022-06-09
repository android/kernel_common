/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_IOMMU_INTERNAL_H
#define __PKVM_IOMMU_INTERNAL_H

#include <../drivers/iommu/intel/iommu.h>
#include <asm/pkvm.h>
#include <asm/pkvm_spinlock.h>
#include "pgtable.h"

struct pkvm_viommu {
	struct pkvm_pgtable pgt;
};

struct pkvm_iommu {
	struct intel_iommu iommu;
	pkvm_spinlock_t lock;
	bool activated;
	struct pkvm_pgtable pgt;
	struct pkvm_viommu viommu;
};

enum sm_level {
	IOMMU_PASID_TABLE = 1,
	IOMMU_PASID_DIR,
	IOMMU_SM_CONTEXT,
	IOMMU_SM_ROOT,
	IOMMU_SM_LEVEL_NUM,
};

#define MAX_NR_PASID_BITS	PKVM_MAX_PASID_BITS

#define PASIDTAB_BITS		6
#define PASIDTAB_SHIFT		0

#define PASIDDIR_BITS		(MAX_NR_PASID_BITS - PASIDTAB_BITS)
#define PASIDDIR_SHIFT		PASIDTAB_BITS

#define DEVFN_BITS		8
#define DEVFN_SHIFT		(PASIDDIR_SHIFT + PASIDDIR_BITS)

#define BUS_BITS		8
#define BUS_SHIFT		(DEVFN_SHIFT + DEVFN_BITS)

/* Used to calculate the level-to-index */
#define SM_DEVFN_BITS		7
#define SM_BUS_BITS		9
#define SM_BUS_SHIFT		(DEVFN_SHIFT + SM_DEVFN_BITS)

#define IOMMU_MAX_VADDR_LEN	(BUS_SHIFT + BUS_BITS)
#define IOMMU_MAX_VADDR		BIT(IOMMU_MAX_VADDR_LEN)

struct pasid_dir_entry {
	u64 val;
};

struct pasid_entry {
	u64 val[8];
};

#endif
