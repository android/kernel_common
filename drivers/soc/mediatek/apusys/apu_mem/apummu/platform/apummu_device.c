// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/module.h>
#include <linux/of_device.h>

#include "../common/apummu_cmn.h"
#include "apummu_plat.h"
#include "apummu_device.h"

static struct apummu_plat mt8196_drv = {
	.slb_wait_time                   = 0,
	.is_general_SLB_support          = true,
	.alloc_DRAM_FB_in_session_create = false,
};

static const struct of_device_id apummu_of_match[] = {
	{ .compatible = "mediatek,rv-apummu-mt8196", .data = &mt8196_drv },
	{ /* end of list */ },
};

const struct of_device_id *apummu_get_of_device_id(void)
{
	return apummu_of_match;
}
