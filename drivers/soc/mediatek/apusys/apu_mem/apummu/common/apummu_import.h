/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_APUMMU_IMPORT_H__
#define __APUSYS_APUMMU_IMPORT_H__
#include <linux/types.h>

#include "linux/soc/mediatek/apummu_mem_def.h"

int apummu_alloc_slb(uint32_t type, uint32_t size, uint32_t slb_wait_time,
			uint64_t *ret_addr, uint64_t *ret_size);
int apummu_free_slb(uint32_t type);
int apummu_check_slb_status(uint32_t type);
#endif
