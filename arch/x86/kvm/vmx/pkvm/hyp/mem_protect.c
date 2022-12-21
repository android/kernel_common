// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/bitfield.h>
#include <pkvm.h>
#include "pkvm_hyp.h"
#include "mem_protect.h"
#include "pgtable.h"

static u64 pkvm_init_invalid_leaf_owner(pkvm_id owner_id)
{
	return FIELD_PREP(PKVM_INVALID_PTE_OWNER_MASK, owner_id);
}

static int host_ept_set_owner_locked(phys_addr_t addr, u64 size, pkvm_id owner_id)
{
	u64 annotation = pkvm_init_invalid_leaf_owner(owner_id);
	int ret;

	ret = pkvm_pgtable_annotate(pkvm_hyp->host_vm.ept, addr, size, annotation);

	return ret;
}
