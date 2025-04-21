// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/sysfs.h>

#include <drm/drm_managed.h>

#include "xe_device.h"
#include "xe_device_sysfs.h"
#include "xe_pm.h"

/**
 * DOC: Xe device sysfs
 * Xe driver requires exposing certain tunable knobs controlled by user space for
 * each graphics device. Considering this, we need to add sysfs attributes at device
 * level granularity.
 * These sysfs attributes will be available under pci device kobj directory.
 *
 * vram_d3cold_threshold - Report/change vram used threshold(in MB) below
 * which vram save/restore is permissible during runtime D3cold entry/exit.
 */

static struct xe_device *
kobj_to_xe(struct kobject *kobj)
{
	struct device *dev = container_of(kobj, struct device, kobj);

	return pdev_to_xe_device(to_pci_dev(dev));
}

static ssize_t
vram_d3cold_threshold_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	struct xe_device *xe = kobj_to_xe(kobj);
	int ret;

	if (!xe)
		return -EINVAL;

	xe_pm_runtime_get(xe);
	ret = sysfs_emit(buf, "%d\n", xe->d3cold.vram_threshold);
	xe_pm_runtime_put(xe);

	return ret;
}

static ssize_t
vram_d3cold_threshold_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buff, size_t count)
{
	struct xe_device *xe = kobj_to_xe(kobj);
	u32 vram_d3cold_threshold;
	int ret;

	if (!xe)
		return -EINVAL;

	ret = kstrtou32(buff, 0, &vram_d3cold_threshold);
	if (ret)
		return ret;

	drm_dbg(&xe->drm, "vram_d3cold_threshold: %u\n", vram_d3cold_threshold);

	xe_pm_runtime_get(xe);
	ret = xe_pm_set_vram_threshold(xe, vram_d3cold_threshold);
	xe_pm_runtime_put(xe);

	return ret ?: count;
}

static struct kobj_attribute attr_vram_d3cold_threshold = __ATTR_RW(vram_d3cold_threshold);

static void xe_device_sysfs_fini(void *arg)
{
	struct xe_device *xe = arg;

	sysfs_remove_file(&xe->drm.dev->kobj, &attr_vram_d3cold_threshold.attr);
}

int xe_device_sysfs_init(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;
	int ret;

	ret = sysfs_create_file(&dev->kobj, &attr_vram_d3cold_threshold.attr);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, xe_device_sysfs_fini, xe);
}
