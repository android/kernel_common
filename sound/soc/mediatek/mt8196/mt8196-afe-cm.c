// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 MediaTek Inc.
 * Author: Darren Ye <darren.ye@mediatek.com>
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#include "mtk-afe-fe-dai.h"
#include "mtk-base-afe.h"

#include "mt8196-afe-cm.h"
#include "mt8196-afe-common.h"

void mt8196_set_cm_rate(struct mtk_base_afe *afe, int id, unsigned int rate)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;

	afe_priv->cm_rate[id] = rate;
}
EXPORT_SYMBOL_GPL(mt8196_set_cm_rate);

static int mt8196_convert_cm_ch(unsigned int ch)
{
	return ch - 1;
}

static unsigned int calculate_cm_update(int rate, int ch)
{
	unsigned int update_val;

	update_val = 26000000 / rate / (ch / 2);
	update_val = update_val * 10 / 7;
	if (update_val > 100)
		update_val = 100;
	if (update_val < 7)
		update_val = 7;

	return update_val;
}

int mt8196_set_cm(struct mtk_base_afe *afe, int id,
	       bool update, bool swap, unsigned int ch)
{
	unsigned int rate = 0;
	unsigned int update_val = 0;
	int reg;
	struct mt8196_afe_private *afe_priv = afe->platform_priv;

	dev_info(afe->dev, "%s()-0, CM%d, rate %d, update %d, swap %d, ch %d\n",
		__func__, id, rate, update, swap, ch);

	rate = afe_priv->cm_rate[id];
	update_val = update ? calculate_cm_update(rate, (int)ch) : 0x64;

	reg = AFE_CM0_CON0 + 0x10 * id;
	/* update cnt */
	mtk_regmap_update_bits(afe->regmap, reg, AFE_CM_UPDATE_CNT_MASK,
							update_val, AFE_CM_UPDATE_CNT_SFT);

	/* rate */
	mtk_regmap_update_bits(afe->regmap, reg, AFE_CM_1X_EN_SEL_FS_MASK,
							rate, AFE_CM_1X_EN_SEL_FS_SFT);

	/* ch num */
	ch = mt8196_convert_cm_ch(ch);
	mtk_regmap_update_bits(afe->regmap, reg, AFE_CM_CH_NUM_MASK,
							ch, AFE_CM_CH_NUM_SFT);

	/* swap */
	mtk_regmap_update_bits(afe->regmap, reg, AFE_CM_BYTE_SWAP_MASK,
							swap, AFE_CM_BYTE_SWAP_SFT);

	return 0;
}
EXPORT_SYMBOL_GPL(mt8196_set_cm);

int mt8196_enable_cm_bypass(struct mtk_base_afe *afe, int id, bool en)
{
	int reg = AFE_CM0_CON0 + 0x10 * id;

	mtk_regmap_update_bits(afe->regmap, reg, AFE_CM_BYPASS_MODE_MASK,
							en, AFE_CM_BYPASS_MODE_SFT);

	return 0;
}
EXPORT_SYMBOL_GPL(mt8196_enable_cm_bypass);

MODULE_DESCRIPTION("Mediatek afe cm");
MODULE_AUTHOR("darren ye <darren.ye@mediatek.com>");
MODULE_LICENSE("GPL");
