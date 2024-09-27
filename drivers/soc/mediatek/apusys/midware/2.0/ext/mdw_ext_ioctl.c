// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/slab.h>
#include <linux/uaccess.h>

#include "mdw_ext_ioctl.h"
#include "mdw_ext.h"
#include "mdw_ext_cmn.h"

long mdw_ext_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned int usize = 0, nr = _IOC_NR(cmd);
	void *kdata = NULL;

	/* check nr before any actions */
	if (nr < MDW_EXT_IOCTL_START || nr > MDW_EXT_IOCTL_END) {
		mdw_drv_debug("not support nr(%u)\n", nr);
		return -ENOTTY;
	}

	/* allocate for user data */
	usize = _IOC_SIZE(cmd);
	/* check kzalloc return */
	kdata = kzalloc(usize, GFP_KERNEL);
	if (unlikely(ZERO_OR_NULL_PTR(kdata)))
		return -ENOMEM;
	/* copy from user data */
	if (cmd & IOC_IN) {
		if (copy_from_user(kdata, (void __user *)arg, usize)) {
			ret = -EFAULT;
			goto out;
		}
	}

	switch (cmd) {
	case MDW_EXT_IOCTL_HS:
		ret = mdw_ext_hs_ioctl(kdata);
		break;
	case MDW_EXT_IOCTL_CMD:
		ret = mdw_ext_cmd_ioctl(kdata);
		break;
	default:
		ret = -EFAULT;
		goto out;
	}

	/* copy to user data */
	if (cmd & IOC_OUT) {
		if (copy_to_user((void __user *)arg, kdata, usize)) {
			ret = -EFAULT;
			goto out;
		}
	}

out:
	kfree(kdata);

	return ret;
}
