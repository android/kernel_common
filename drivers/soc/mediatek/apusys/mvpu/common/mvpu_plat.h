/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __MVPU_PLAT_API_H__
#define __MVPU_PLAT_API_H__

#include <linux/kernel.h>
#include <linux/platform_device.h>

#include "linux/soc/mediatek/mtk_apu_device.h"

#define MAX_CORE_NUM 2
#define PREEMPT_L1_BUFFER (512 * 1024)
#define PREEMPT_ITCM_BUFFER (128 * 1024)

enum MVPU_IPI_TYPE {
	/* set uP log level */
	MVPU_IPI_LOG_LEVEL,
	/* uP exception msg */
	MVPU_IPI_MICROP_MSG
};

enum MVPU_IPI_DIR_TYPE {
	MVPU_IPI_READ,
	MVPU_IPI_WRITE,
};

enum MVPU_SW_VERSION {
	MVPU_SW_VER_MVPU20 = 0,
	MVPU_SW_VER_MVPU25,
	MVPU_SW_VER_MVPU25a,
	MVPU_SW_VER_MVPU3,
};

struct mvpu_sec_ops {
	int (*mvpu_sec_init)(struct device *dev);
	int (*mvpu_load_img)(struct device *dev);
	int (*mvpu_sec_sysfs_init)(struct kobject *root_dir);
};


struct mvpu_ops {
	int (*mvpu_ipi_init)(void);
	void (*mvpu_ipi_deinit)(void);
	int (*mvpu_ipi_send)(uint32_t type, uint32_t dir, uint64_t *val);
	void (*mvpu_handler_lite_init)(void);
	int (*mvpu_handler_lite)(int type, void *hnd, struct apusys_device *dev);
};

struct mvpu_preempt_buffer {
	uint32_t *itcm_kernel_addr_core_0[5];
	uint32_t *l1_kernel_addr_core_0[5];
	uint32_t *itcm_kernel_addr_core_1[5];
	uint32_t *l1_kernel_addr_core_1[5];
};

struct mvpu_platdata {
	struct platform_device *pdev;
	const struct mvpu_ops *ops;
	const struct mvpu_sec_ops *sec_ops;
	enum MVPU_SW_VERSION sw_ver;
	struct mvpu_preempt_buffer preempt_buffer;
	uint32_t core_num;
	uint32_t sw_preemption_level;
	uint64_t dma_mask;
};

int mvpu_platdata_init(struct platform_device *pdev);
const struct of_device_id *mvpu_plat_get_device(void);

extern struct mvpu_platdata mvpu_mt8196_platdata;

extern struct mvpu_platdata *g_mvpu_platdata;
extern int mvpu_drv_loglv;

#endif /* __MVPU_PLAT_API_H__ */
