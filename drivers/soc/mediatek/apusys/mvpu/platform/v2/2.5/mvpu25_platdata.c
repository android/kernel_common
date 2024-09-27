// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */
#include "mvpu_plat.h"
#include "mvpu25_ipi.h"
#include "mvpu25_handler.h"
#include "mvpu25_sec.h"

struct mvpu_sec_ops mvpu25_sec_ops = {
	.mvpu_sec_init = mvpu25_sec_init,
	.mvpu_load_img = mvpu25_load_img,
	.mvpu_sec_sysfs_init = mvpu25_sec_sysfs_init
};

struct mvpu_ops mvpu25_ops = {
	.mvpu_ipi_init = mvpu25_ipi_init,
	.mvpu_ipi_deinit = mvpu25_ipi_deinit,
	.mvpu_ipi_send = mvpu25_ipi_send,
	.mvpu_handler_lite_init = mvpu25_handler_lite_init,
	.mvpu_handler_lite = mvpu25_handler_lite,
};

struct mvpu_ops mvpu25a_ops = {
	.mvpu_ipi_init = mvpu25a_ipi_init,
	.mvpu_ipi_deinit = mvpu25a_ipi_deinit,
	.mvpu_ipi_send = mvpu25_ipi_send,
	.mvpu_handler_lite_init = mvpu25_handler_lite_init,
	.mvpu_handler_lite = mvpu25_handler_lite,
};

struct mvpu_platdata mvpu_mt8196_platdata = {
	.sw_preemption_level = 1,
	.sw_ver = MVPU_SW_VER_MVPU25a,
	.ops = &mvpu25a_ops,
	.sec_ops = &mvpu25_sec_ops
};
