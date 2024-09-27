/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_APUMMU_DRV_H__
#define __APUSYS_APUMMU_DRV_H__

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/wait.h>

/* config define */
#define DRAM_FALL_BACK_IN_RUNTIME	(1)

/* for RV data*/
struct apummu_remote_data {
#if !(DRAM_FALL_BACK_IN_RUNTIME)
	uint64_t dram[32];
#endif
	uint32_t dram_max;

	uint32_t vlm_size;
	uint32_t vlm_addr;

	uint32_t SLB_base_addr;
	uint32_t SLB_EXT_addr;
	uint32_t SLB_size;

	uint32_t TCM_base_addr;
	uint32_t general_SRAM_size; // TCM + general SLB
};

/* for plat data */
struct apummu_platform {
	uint32_t slb_wait_time;
	uint32_t boundary;

	uint32_t internal_SLB_cnt;
	uint32_t external_SLB_cnt;
	struct mutex slb_mtx;

	bool is_general_SLB_support;
	bool alloc_DRAM_FB_in_session_create; // add for DRAM FB alloc time check
};

struct apummu_resource {
	uint64_t iova;
	uint32_t size;
	void *base;
};

struct apummu_resource_mgt {
	struct apummu_resource vlm_dram;
	struct apummu_resource internal_SLB;
	struct apummu_resource external_SLB;
	struct apummu_resource genernal_SLB;
};

/* apummu driver's private structure */
struct apummu_dev_info {
	bool init_done;
	struct device *dev;
	dev_t apummu_devt;
	struct cdev apummu_cdev;
	struct rpmsg_device *rpdev;

	struct apummu_resource_mgt rsc;

	struct apummu_platform plat;
	struct apummu_remote_data remote;
};

/* for aputop module AEE data */
struct apummu_rx_data {
	uint8_t module_id;
	int8_t  error_code;
};
enum {
	APUMMU_RX_TEST = 0,
	APUMMU_RX_APUMMU_AEE = 1,
	APUMMU_RX_HSE_AEE = 2,
	APUMMU_RX_CBFC_AEE = 3,
	APUMMU_RX_LAST = 4,
};

extern struct apummu_tbl g_ammu_table_set;

#endif
