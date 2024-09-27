/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef __MTK_APUEXT_H__
#define __MTK_APUEXT_H__

#include <linux/file.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/types.h>

#define MDW_EXT_IDR_MIN (1)
#define MDW_EXT_IDR_MAX (4096)

struct mdw_ext_device {
	struct miscdevice *misc_dev;
	/* ext operation */
	struct idr ext_ids;
	struct mutex ext_mtx;
};

long mdw_ext_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
int mdw_ext_cmd_ioctl(void *data);
int mdw_ext_hs_ioctl(void *data);

#endif
