// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */
#include <linux/kernel.h>
#include <linux/mutex.h>

#include "linux/soc/mediatek/mtk_apu_device.h"

#include "mvpu_plat.h"
#include "mvpu25_handler.h"
#include "mvpu25_valid.h"
#include "mvpu25_sec.h"

struct mutex mvpu25_pool_lock;

void mvpu25_handler_lite_init(void)
{
	mutex_init(&mvpu25_pool_lock);
}


int mvpu25_handler_lite(int type, void *hnd, struct apusys_device *dev)
{
	int ret = 0;

	if (!dev)
		return -1;

	if (!hnd) {
		pr_info("%s get hnd fail\n", __func__);
		return -1;
	}

	if (dev->dev_type != APUSYS_DEVICE_MVPU) {
		pr_info("%s get wrong dev_type: %d\n", __func__, dev->dev_type);
		return -1;
	}

	if (mvpu_drv_loglv >= APUSYS_MVPU_LOG_DBG)
		pr_info("[MVPU][Sec] APU CMD type %d\n", type);

	switch (type) {
	case APUSYS_CMD_VALIDATE:
		ret = mvpu25_validation(hnd);
		break;
	case APUSYS_CMD_SESSION_CREATE:
		mvpu25_set_sec_log_lvl(mvpu_drv_loglv);
		ret = 0;
		break;
	case APUSYS_CMD_SESSION_DELETE:
		if (hnd == NULL) {
			pr_info("[MVPU][Sec] APUSYS_CMD_SESSION_DELETE error: hnd is NULL\n");
			ret = -1;
		} else {
			mutex_lock(&mvpu25_pool_lock);
			mvpu25_clear_session(hnd);
			mutex_unlock(&mvpu25_pool_lock);
			ret = 0;
		}
		break;
	default:
		pr_info("%s get wrong cmd type: %d\n", __func__, type);
		ret = -1;
		break;
	}

	return ret;
}
