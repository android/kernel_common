/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __MTK_APU_MDW_EXT_CMN_H__
#define __MTK_APU_MDW_EXT_CMN_H__

#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/time.h>

#include "mdw_cmn.h"
#include "mdw_ext.h"

enum {
	MDWEXT_DBG_DRV = 0x01,
	MDWEXT_DBG_FLW = 0x02,
	MDWEXT_DBG_CMD = 0x04,
	MDWEXT_DBG_VLD = 0x40,
};
extern struct mdw_ext_device *mdw_ext_dev;

static inline
int mdwext_debug_on(int mask)
{
	return g_mdw_klog & mask;
}

#define mdwext_debug(mask, x, ...) do { if (mdwext_debug_on(mask)) \
		dev_info(mdw_ext_dev->misc_dev->this_device, \
		"[debug] %s/%d " x, __func__, __LINE__, ##__VA_ARGS__); \
	} while (0)

#define mdwext_drv_err(x, args...) \
	dev_info(mdw_ext_dev->misc_dev->this_device, \
	"[error] %s " x, __func__, ##args)
#define mdwext_drv_warn(x, args...) \
	dev_info(mdw_ext_dev->misc_dev->this_device, \
	"[warn] %s " x, __func__, ##args)
#define mdwext_drv_info(x, args...) \
	dev_info(mdw_ext_dev->misc_dev->this_device, \
	"%s " x, __func__, ##args)

#define mdwext_drv_debug(x, ...) mdwext_debug(MDWEXT_DBG_DRV, x, ##__VA_ARGS__)
#define mdwext_flw_debug(x, ...) mdwext_debug(MDWEXT_DBG_FLW, x, ##__VA_ARGS__)
#define mdwext_cmd_debug(x, ...) mdwext_debug(MDWEXT_DBG_CMD, x, ##__VA_ARGS__)
#define mdwext_vld_debug(x, ...) mdwext_debug(MDWEXT_DBG_VLD, x, ##__VA_ARGS__)


#endif
