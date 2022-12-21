// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_MEM_PROTECT_H__
#define __PKVM_MEM_PROTECT_H__

/* use 20 bits[12~31] - not conflict w/ low 12 bits pte prot */
#define PKVM_INVALID_PTE_OWNER_MASK	GENMASK(31, 12)

typedef u32 pkvm_id;

#define OWNER_ID_HYP	0UL
#define OWNER_ID_HOST	1UL
#define OWNER_ID_INV	(~(u32)0UL)

#endif
