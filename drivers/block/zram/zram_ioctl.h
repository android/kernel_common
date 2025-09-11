/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZRAM_IOCTL_H_
#define _ZRAM_IOCTL_H_

#if IS_ENABLED(CONFIG_ZRAM_ANDROID_IOCTL)
int zram_ioctl(struct block_device *bdev, blk_mode_t mode,
	       unsigned int cmd, unsigned long arg);
#else
inline int zram_ioctl(struct block_device *bdev, blk_mode_t mode,
		      unsigned int cmd, unsigned long arg)
{
	return -EINVAL;
}
#endif

#endif /* _ZRAM_IOCTL_H_ */

