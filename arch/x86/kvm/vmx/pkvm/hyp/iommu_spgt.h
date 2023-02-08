/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2022 Intel Corporation.
 * Copyright(c) 2023 Semihalf.
 */

#include "pgtable.h"

struct pkvm_iommu_spgt {
	int refcount;
	int noncoherent_count;
	struct hlist_node hnode;
	unsigned long root_gpa;
	unsigned long index;
	struct pkvm_pgtable pgt;
};

struct pkvm_pgtable *pkvm_get_host_iommu_spgt(unsigned long root_gpa, bool coherency);
void pkvm_put_host_iommu_spgt(struct pkvm_pgtable *spgt, bool coherency);
