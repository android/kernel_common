/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 MediaTek Inc.
 * Author: Darren Ye <darren.ye@mediatek.com>
 */

#ifndef MTK_AFE_CM_H_
#define MTK_AFE_CM_H_
enum {
	CM0,
	CM1,
	CM2,
	CM_NUM,
};

void mt8196_set_cm_rate(struct mtk_base_afe *afe, int id, unsigned int rate);

int mt8196_set_cm(struct mtk_base_afe *afe, int id, bool update,
				bool swap, unsigned int ch);
int mt8196_enable_cm_bypass(struct mtk_base_afe *afe, int id, bool en);

#endif /* MTK_AFE_CM_H_ */

