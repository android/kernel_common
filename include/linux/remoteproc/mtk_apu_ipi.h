/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef MTK_APU_IPI_H
#define MTK_APU_IPI_H

#include <linux/rpmsg/mtk_rpmsg.h>

#define MTK_APU_FW_VER_LEN		(250)
#define MTK_APU_SHARE_BUFFER_SIZE	(256)
#define MTK_APU_SHARE_BUF_SIZE		(round_up(sizeof(struct mtk_share_obj)*2, PAGE_SIZE))

struct mtk_apu;

enum {
	MTK_APU_IPI_INIT = 0,
	MTK_APU_IPI_NS_SERVICE,
	MTK_APU_IPI_DEEP_IDLE,
	MTK_APU_IPI_CTRL_RPMSG,
	MTK_APU_IPI_MIDDLEWARE,
	MTK_APU_IPI_REVISER_RPMSG,
	MTK_APU_IPI_PWR_TX,	// cmd direction from ap to up
	MTK_APU_IPI_PWR_RX,	// cmd direction from up to ap
	MTK_APU_IPI_MDLA_TX,
	MTK_APU_IPI_MDLA_RX,
	MTK_APU_IPI_TIMESYNC,
	MTK_APU_IPI_EDMA_TX,
	MTK_APU_IPI_MNOC_TX,
	MTK_APU_IPI_MVPU_TX,
	MTK_APU_IPI_MVPU_RX,
	MTK_APU_IPI_LOG_LEVEL,
	MTK_APU_IPI_APS_TX,
	MTK_APU_IPI_APS_RX,
	MTK_APU_IPI_SAPU_LOCK,
	MTK_APU_IPI_SCP_MIDDLEWARE,
	MTK_APU_IPI_SCP_NP_RECOVER,
	MTK_APU_IPI_UT,
	MTK_APU_IPI_APUMMU_RPMSG,
	MTK_APU_IPI_EDMA_RX,
	MTK_APU_IPI_APUMMU_RX,
	MTK_APU_IPI_MAX,
};

struct mtk_apu_run {
	s8 fw_ver[MTK_APU_FW_VER_LEN];
	u32 signaled;
	wait_queue_head_t wq;
};

struct mtk_apu_ipi_desc {
	struct mutex lock;
	ipi_top_handler_t top_handler;
	ipi_handler_t handler;
	void *priv;

	/*
	 * positive: host-initiated ipi outstanding count
	 * negative: apu-initiated ipi outstanding count
	 */
	int usage_cnt;
};

struct mtk_share_obj {
	u8 share_buf[MTK_APU_SHARE_BUFFER_SIZE];
};

struct mtk_ipi_task {
	int id;
	int len;
};

void mtk_apu_ipi_remove(struct mtk_apu *apu);
int mtk_apu_ipi_init(struct platform_device *pdev, struct mtk_apu *apu);
int mtk_apu_ipi_register(struct mtk_apu *apu, u32 id, ipi_top_handler_t top_handler,
			 ipi_handler_t handler, void *priv);
void mtk_apu_ipi_unregister(struct mtk_apu *apu, u32 id);
int mtk_apu_ipi_send(struct mtk_apu *apu, u32 id, void *data, u32 len, u32 wait_ms);
int mtk_apu_power_on_off(struct platform_device *pdev, u32 id, u32 on, u32 off);

#endif /* MTK_APU_IPI_H */
