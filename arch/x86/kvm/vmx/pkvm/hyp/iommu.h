/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_IOMMU_H_
#define _PKVM_IOMMU_H_

int pkvm_init_iommu(void);
unsigned long pkvm_access_iommu(bool is_read, int len, unsigned long reg, unsigned long val);

#endif
