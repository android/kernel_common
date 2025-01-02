// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/container_of.h>
#include <linux/dev_printk.h>
#include <linux/sched/clock.h>
#include <linux/workqueue.h>

#include <linux/remoteproc/mtk_apu.h>

static void mtk_apu_send_timesync_request(struct mtk_apu *apu)
{
	int ret;
	u64 timesync_stamp;

	timesync_stamp = sched_clock();
	ret = mtk_apu_ipi_send(apu, MTK_APU_IPI_TIMESYNC, &timesync_stamp, sizeof(u64), 0);

	if (ret)
		dev_err(apu->dev, "%s: mtk_apu_ipi_send fail(%d)\n", __func__, ret);
}

static void mtk_apu_timesync_handler(void *data, u32 len, void *priv)
{
	struct mtk_apu *apu = (struct mtk_apu *)priv;

	dev_dbg(apu->dev, "%s: timesync request received\n", __func__);
	mtk_apu_send_timesync_request(apu);
}

int mtk_apu_timesync_init(struct mtk_apu *apu)
{
	int ret;

	ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_TIMESYNC, NULL, mtk_apu_timesync_handler, apu);
	if (ret) {
		dev_err(apu->dev, "%s: failed to register IPI\n", __func__);
		return ret;
	}

	return 0;
}

void mtk_apu_timesync_remove(struct mtk_apu *apu)
{
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_TIMESYNC);
}
