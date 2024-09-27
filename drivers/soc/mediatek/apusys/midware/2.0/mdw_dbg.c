// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/uaccess.h>

#include "mdw_cmn.h"

struct dentry *mdw_dbg_root;
struct dentry *mdw_dbg_trace;
struct dentry *mdw_dbg_device;

u32 g_mdw_klog;
u8 cfg_apusys_trace;
u32 g_mdw_pwroff_cnt;
u32 g_mdw_poll_interval;
u32 g_mdw_poll_timeout;

/* default value need to align with rv or add sync flow */
u64 dbg_min_dtime;
u64 dbg_max_dtime;

static int min_etime_set(void *data, u64 val)
{
	struct mdw_device *mdev = (struct mdw_device *)data;

	mdev->plat_funcs->set_param(mdev, MDW_INFO_MIN_ETIME, val);
	return 0;
}
static int min_etime_get(void *data, u64 *val)
{
	struct mdw_device *mdev = (struct mdw_device *)data;

	*val = mdev->plat_funcs->get_info(mdev, MDW_INFO_MIN_ETIME);
	return 0;
}

static int min_dtime_set(void *data, u64 val)
{
	struct mdw_device *mdev = (struct mdw_device *)data;

	dbg_min_dtime = val;
	mdev->plat_funcs->set_param(mdev, MDW_INFO_MIN_DTIME, val);
	return 0;
}
static int min_dtime_get(void *data, u64 *val)
{
	struct mdw_device *mdev = (struct mdw_device *)data;

	*val = mdev->plat_funcs->get_info(mdev, MDW_INFO_MIN_DTIME);
	return 0;
}

static int max_dtime_set(void *data, u64 val)
{
	struct mdw_device *mdev = (struct mdw_device *)data;

	dbg_max_dtime = val;
	mdev->plat_funcs->set_param(mdev, MDW_INFO_MAX_DTIME, val);
	return 0;
}

static int max_dtime_get(void *data, u64 *val)
{
	struct mdw_device *mdev = (struct mdw_device *)data;

	*val = mdev->plat_funcs->get_info(mdev, MDW_INFO_MAX_DTIME);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_min_dtime, min_dtime_get, min_dtime_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(fops_max_dtime, max_dtime_get, max_dtime_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(fops_min_etime, min_etime_get, min_etime_set, "%llu\n");
//----------------------------------------------
int mdw_dbg_init(struct mdw_device *mdev, struct dentry *dbg_root)
{
	g_mdw_klog = 0x0;
	g_mdw_pwroff_cnt = 0x0;
	g_mdw_poll_interval = 0x0;
	g_mdw_poll_timeout = 0x0;

	/* create debug root */
	mdw_dbg_root = debugfs_create_dir("midware", dbg_root);

	/* create log level */
	debugfs_create_u32("klog", 0644,
		mdw_dbg_root, &g_mdw_klog);

	/* create log level */
	cfg_apusys_trace = 0;
	debugfs_create_u8("trace_en", 0644,
		mdw_dbg_root, &cfg_apusys_trace);

	/* create pwroff_cnt */
	debugfs_create_u32("pwroff_cnt", 0644,
		mdw_dbg_root, &g_mdw_pwroff_cnt);

	/* create poll_time */
	debugfs_create_u32("poll_interval", 0644,
		mdw_dbg_root, &g_mdw_poll_interval);
	/* create poll_timeout */
	debugfs_create_u32("poll_timeout", 0644,
		mdw_dbg_root, &g_mdw_poll_timeout);

	dbg_min_dtime = 0;
	dbg_max_dtime = 10000;
	debugfs_create_file("min_dtime", 0644, mdw_dbg_root, mdw_dev, &fops_min_dtime);
	debugfs_create_file("max_dtime", 0644, mdw_dbg_root, mdw_dev, &fops_max_dtime);
	debugfs_create_file("min_etime", 0644, mdw_dbg_root, mdw_dev, &fops_min_etime);

	mdw_dbg_device = debugfs_create_dir("device", mdw_dbg_root);

	return 0;
}

void mdw_dbg_deinit(void)
{
}
