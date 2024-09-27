// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>

#include "mvpu_plat.h"
#include "mvpu_sysfs.h"

static struct kobject *root_dir;

static ssize_t loglevel_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	uint64_t level = 0;
	int ret = 0;

	g_mvpu_platdata->ops->mvpu_ipi_send(MVPU_IPI_LOG_LEVEL, MVPU_IPI_READ, &level);

	pr_info("[MVPU] %s, level= %llu\n", __func__, level);
	ret = sprintf(buf, "mvpu log level= %llu\n", level);

	if (ret < 0) {
		pr_info("[MVPU] %s, sprintf fail(%d)\n", __func__, ret);
		return 0;
	}

	return ret;
}

static ssize_t loglevel_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *cmd, size_t count)
{
	uint64_t ret = 0;
	uint64_t level = 0;

	ret = kstrtoull(cmd, 16, &level);

	if (!ret) {
		pr_info("[MVPU] %s, level= %llu\n", __func__, level);

		g_mvpu_platdata->ops->mvpu_ipi_send(MVPU_IPI_LOG_LEVEL, MVPU_IPI_WRITE, &level);

		mvpu_drv_loglv = level;
	} else {
		pr_info("[MVPU] %s[%d]: get invalid cmd\n", __func__, __LINE__);
	}
	return count;

}

static struct kobj_attribute loglevel = {
	.attr = {
		.name = "loglevel",
		.mode = 0644,
	},
	.show = loglevel_show,
	.store = loglevel_store,
};

int mvpu_sysfs_init(void)
{
	int ret = 0;

	pr_info("%s +\n", __func__);

	mvpu_drv_loglv = 0;

	/* create /sys/kernel/mvpu */
	root_dir = kobject_create_and_add("mvpu", kernel_kobj);
	if (!root_dir) {
		pr_info("%s kobject_create_and_add fail\n", __func__);
		return -EINVAL;
	}

	if (sysfs_create_file(root_dir, &loglevel.attr)) {
		pr_info("%s sysfs_create_file fail\n", __func__);
		return -EINVAL;
	}

	ret = g_mvpu_platdata->sec_ops->mvpu_sec_sysfs_init(root_dir);

	return ret;
}

void mvpu_sysfs_exit(void)
{
	kobject_del(root_dir);
}
