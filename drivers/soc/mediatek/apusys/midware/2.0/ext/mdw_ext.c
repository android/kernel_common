// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include "mdw.h"
#include "mdw_ext.h"
#include "mdw_ext_cmn.h"
#include "mdw_ext_export.h"
#include "mdw_ext_ioctl.h"

#define MDWEXT_DEV_NAME "apuext"

struct mdw_ext_device *mdw_ext_dev;

void mdw_ext_lock(void)
{
	mutex_lock(&mdw_ext_dev->ext_mtx);
}

void mdw_ext_unlock(void)
{
	mutex_unlock(&mdw_ext_dev->ext_mtx);
}

static int mdw_ext_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int mdw_ext_close(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations mdw_ext_fops = {
	.owner = THIS_MODULE,
	.open = mdw_ext_open,
	.release = mdw_ext_close,
	.unlocked_ioctl = mdw_ext_ioctl,
	.compat_ioctl = mdw_ext_ioctl,
};

static struct miscdevice mdw_ext_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = MDWEXT_DEV_NAME,
	.fops = &mdw_ext_fops,
};

//----------------------------------------
int mdw_ext_init(struct mdw_device *mdw_dev)
{
	int ret = -EINVAL;

	/* check support ext by version */
	if (mdw_dev) {
		if (mdw_dev->mdw_ver < 4) {
			ret = 0;
			goto out;
		}
	}

	pr_info("%s register misc...\n", __func__);
	if (misc_register(&mdw_ext_misc_dev)) {
		pr_info("failed to register apu ext misc driver\n");
		goto out;
	}

	/* alloc ext device */
	mdw_ext_dev = kzalloc(sizeof(*mdw_ext_dev), GFP_KERNEL);
	if (mdw_ext_dev == NULL)
		goto deregister_ext;

	mdw_ext_dev->misc_dev = &mdw_ext_misc_dev;
	idr_init(&mdw_ext_dev->ext_ids);
	mutex_init(&mdw_ext_dev->ext_mtx);
	pr_info("%s register misc done\n", __func__);

	ret = 0;

	goto out;

deregister_ext:
	misc_deregister(&mdw_ext_misc_dev);
out:
	return ret;
}

void mdw_ext_deinit(void)
{
	/* free ext driver */
	if (mdw_ext_dev != NULL) {
		misc_deregister(&mdw_ext_misc_dev);
		kfree(mdw_ext_dev);
	}
}
