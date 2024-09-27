/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */
#ifndef __MVPU25_HANDLER_H__
#define __MVPU25_HANDLER_H__

#include <linux/mutex.h>
#include <linux/kernel.h>
#include "linux/soc/mediatek/mtk_apu_device.h"

extern struct mutex mvpu25_pool_lock;

void mvpu25_handler_lite_init(void);
int mvpu25_handler_lite(int type, void *hnd, struct apusys_device *dev);

#endif /* __MVPU25_HANDLER_H__ */
