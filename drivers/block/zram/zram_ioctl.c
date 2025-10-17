// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_ioctl"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>

#include "zram_drv.h"
#include "zram_ioctl.h"

int zram_ioctl(struct block_device *bdev, blk_mode_t mode,
	       unsigned int cmd, unsigned long arg)
{
	return -EINVAL;
}
