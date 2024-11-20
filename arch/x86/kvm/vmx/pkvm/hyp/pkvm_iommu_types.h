/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_IOMMU_TYPES_H
#define __PKVM_IOMMU_TYPES_H

#include <../drivers/iommu/intel/iommu.h>

#define PKVM_QI_DESC_ALIGNED_SIZE		ALIGN(QI_LENGTH * sizeof(struct qi_desc), PAGE_SIZE)
#define PKVM_QI_DESC_STATUS_ALIGNED_SIZE	ALIGN(QI_LENGTH * sizeof(int), PAGE_SIZE)

#endif
