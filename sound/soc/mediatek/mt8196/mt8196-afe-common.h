/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt8196-afe-common.h  --  Mediatek 8196 audio driver definitions
 *
 * Copyright (c) 2024 MediaTek Inc.
 *  Author: Darren Ye <darren.ye@mediatek.com>
 */

#ifndef _MT_6991_AFE_COMMON_H_
#define _MT_6991_AFE_COMMON_H_
#include <sound/soc.h>
#include <sound/pcm.h>
#include <linux/clk.h>
#include <linux/list.h>
#include <linux/regmap.h>
#include "mt8196-reg.h"
#include "mtk-base-afe.h"
#include "mt8196-afe-cm.h"

#define AUDIO_AEE(...) WARN_ON(true)

enum {
	MTK_AFE_RATE_8K,
	MTK_AFE_RATE_11K,
	MTK_AFE_RATE_12K,
	MTK_AFE_RATE_384K,
	MTK_AFE_RATE_16K,
	MTK_AFE_RATE_22K,
	MTK_AFE_RATE_24K,
	MTK_AFE_RATE_352K,
	MTK_AFE_RATE_32K,
	MTK_AFE_RATE_44K,
	MTK_AFE_RATE_48K,
	MTK_AFE_RATE_88K,
	MTK_AFE_RATE_96K,
	MTK_AFE_RATE_176K,
	MTK_AFE_RATE_192K,
	MTK_AFE_RATE_260K,
};
/* HW IPM 2.0 */
enum {
	MTK_AFE_IPM2P0_RATE_8K = 0x0,
	MTK_AFE_IPM2P0_RATE_11K = 0x1,
	MTK_AFE_IPM2P0_RATE_12K = 0x2,
	MTK_AFE_IPM2P0_RATE_16K = 0x4,
	MTK_AFE_IPM2P0_RATE_22K = 0x5,
	MTK_AFE_IPM2P0_RATE_24K = 0x6,
	MTK_AFE_IPM2P0_RATE_32K = 0x8,
	MTK_AFE_IPM2P0_RATE_44K = 0x9,
	MTK_AFE_IPM2P0_RATE_48K = 0xa,
	MTK_AFE_IPM2P0_RATE_88K = 0xd,
	MTK_AFE_IPM2P0_RATE_96K = 0xe,
	MTK_AFE_IPM2P0_RATE_176K = 0x11,
	MTK_AFE_IPM2P0_RATE_192K = 0x12,
	MTK_AFE_IPM2P0_RATE_352K = 0x15,
	MTK_AFE_IPM2P0_RATE_384K = 0x16,
};

enum {
	MTK_AFE_PCM_RATE_8K,
	MTK_AFE_PCM_RATE_16K,
	MTK_AFE_PCM_RATE_32K,
	MTK_AFE_PCM_RATE_48K,
};

enum {
	MTKAIF_PROTOCOL_1 = 0,
	MTKAIF_PROTOCOL_2,
	MTKAIF_PROTOCOL_2_CLK_P2,
};

enum {
	MT8196_MEMIF_DL0,
	MT8196_MEMIF_DL1,
	MT8196_MEMIF_DL2,
	MT8196_MEMIF_DL3,
	MT8196_MEMIF_DL4,
	MT8196_MEMIF_DL5,
	MT8196_MEMIF_DL6,
	MT8196_MEMIF_DL7,
	MT8196_MEMIF_DL8,
	MT8196_MEMIF_DL23,
	MT8196_MEMIF_DL24,
	MT8196_MEMIF_DL25,
	MT8196_MEMIF_DL26,
	MT8196_MEMIF_DL_4CH,
	MT8196_MEMIF_DL_24CH,
	MT8196_MEMIF_VUL0,
	MT8196_MEMIF_VUL1,
	MT8196_MEMIF_VUL2,
	MT8196_MEMIF_VUL3,
	MT8196_MEMIF_VUL4,
	MT8196_MEMIF_VUL5,
	MT8196_MEMIF_VUL6,
	MT8196_MEMIF_VUL7,
	MT8196_MEMIF_VUL8,
	MT8196_MEMIF_VUL9,
	MT8196_MEMIF_VUL10,
	MT8196_MEMIF_VUL24,
	MT8196_MEMIF_VUL25,
	MT8196_MEMIF_VUL26,
	MT8196_MEMIF_VUL_CM0,
	MT8196_MEMIF_VUL_CM1,
	MT8196_MEMIF_VUL_CM2,
	MT8196_MEMIF_ETDM_IN0,
	MT8196_MEMIF_ETDM_IN1,
	MT8196_MEMIF_ETDM_IN2,
	MT8196_MEMIF_ETDM_IN3,
	MT8196_MEMIF_ETDM_IN4,
	MT8196_MEMIF_ETDM_IN6,
	MT8196_MEMIF_HDMI,
	MT8196_MEMIF_NUM,
	MT8196_DAI_ADDA = MT8196_MEMIF_NUM,
	MT8196_DAI_ADDA_CH34,
	MT8196_DAI_ADDA_CH56,
	MT8196_DAI_AP_DMIC,
	MT8196_DAI_AP_DMIC_CH34,
	MT8196_DAI_AP_DMIC_MULTICH,
	MT8196_DAI_VOW,
	MT8196_DAI_VOW_SCP_DMIC,
	MT8196_DAI_CONNSYS_I2S,
	MT8196_DAI_I2S_IN0,  //48
	MT8196_DAI_I2S_IN1,
	MT8196_DAI_I2S_IN2,
	MT8196_DAI_I2S_IN3,
	MT8196_DAI_I2S_IN4,
	MT8196_DAI_I2S_IN6,
	MT8196_DAI_I2S_OUT0,
	MT8196_DAI_I2S_OUT1,
	MT8196_DAI_I2S_OUT2,
	MT8196_DAI_I2S_OUT3,
	MT8196_DAI_I2S_OUT4,
	MT8196_DAI_I2S_OUT6,
	MT8196_DAI_FM_I2S_MASTER,
	MT8196_DAI_HW_GAIN_0,
	MT8196_DAI_HW_GAIN_1,
	MT8196_DAI_HW_GAIN_2,
	MT8196_DAI_HW_GAIN_3,
	MT8196_DAI_SRC_0,
	MT8196_DAI_SRC_1,
	MT8196_DAI_SRC_2,
	MT8196_DAI_SRC_3,
	MT8196_DAI_PCM_0,
	MT8196_DAI_PCM_1,
	MT8196_DAI_TDM,
	MT8196_DAI_TDM_DPTX,
	MT8196_DAI_HOSTLESS_LPBK,
	MT8196_DAI_HOSTLESS_FM,
	MT8196_DAI_HOSTLESS_HW_GAIN_AAUDIO,
	MT8196_DAI_HOSTLESS_SRC_AAUDIO,
	MT8196_DAI_HOSTLESS_SPEECH,
	MT8196_DAI_HOSTLESS_BT,
	MT8196_DAI_HOSTLESS_SPH_ECHO_REF,
	MT8196_DAI_HOSTLESS_SPK_INIT,
	MT8196_DAI_HOSTLESS_IMPEDANCE,
	MT8196_DAI_HOSTLESS_SRC_0,
	MT8196_DAI_HOSTLESS_SRC_1,
	MT8196_DAI_HOSTLESS_SRC_2,
	MT8196_DAI_HOSTLESS_HW_SRC_0_OUT,
	MT8196_DAI_HOSTLESS_HW_SRC_0_IN,
	MT8196_DAI_HOSTLESS_HW_SRC_1_OUT,
	MT8196_DAI_HOSTLESS_HW_SRC_1_IN,
	MT8196_DAI_HOSTLESS_HW_SRC_2_OUT,
	MT8196_DAI_HOSTLESS_HW_SRC_2_IN,
	MT8196_DAI_HOSTLESS_SRC_BARGEIN,
	MT8196_DAI_HOSTLESS_UL1,
	MT8196_DAI_HOSTLESS_UL2,
	MT8196_DAI_HOSTLESS_UL3,
	MT8196_DAI_HOSTLESS_UL4,
	MT8196_DAI_HOSTLESS_DSP_DL,
	MT8196_DAI_NUM,
	/* use for mtkaif calibration specail setting */
	MT8196_DAI_MTKAIF = MT8196_DAI_NUM,
	MT8196_DAI_MISO_ONLY,
};

#define MT8196_DAI_I2S_MAX_NUM 13 //depends each platform's max i2s num

/* update irq ID (= enum) from AFE_IRQ_MCU_STATUS */
enum {
	MT8196_IRQ_0,
	MT8196_IRQ_1,
	MT8196_IRQ_2,
	MT8196_IRQ_3,
	MT8196_IRQ_4,
	MT8196_IRQ_5,
	MT8196_IRQ_6,
	MT8196_IRQ_7,
	MT8196_IRQ_8,
	MT8196_IRQ_9,
	MT8196_IRQ_10,
	MT8196_IRQ_11,
	MT8196_IRQ_12,
	MT8196_IRQ_13,
	MT8196_IRQ_14,
	MT8196_IRQ_15,
	MT8196_IRQ_16,
	MT8196_IRQ_17,
	MT8196_IRQ_18,
	MT8196_IRQ_19,
	MT8196_IRQ_20,
	MT8196_IRQ_21,
	MT8196_IRQ_22,
	MT8196_IRQ_23,
	MT8196_IRQ_24,
	MT8196_IRQ_25,
	MT8196_IRQ_26,
	MT8196_IRQ_31,  /* used only for TDM */
	MT8196_IRQ_NUM,
};
/* update irq ID (= enum) from AFE_IRQ_MCU_STATUS */
enum {
	MT8196_CUS_IRQ_TDM = 0,  /* used only for TDM */
	MT8196_CUS_IRQ_NUM,
};

/* MCLK */
enum {
	MT8196_I2SIN0_MCK = 0,
	MT8196_I2SIN1_MCK,
	MT8196_FMI2S_MCK,
	MT8196_TDMOUT_MCK,
	MT8196_TDMOUT_BCK,
	MT8196_MCK_NUM,
};

struct mt8196_afe_private {
	struct clk **clk;
	struct regmap *cksys_ck;
	struct regmap *vlp_ck;
	struct regmap *pmic_regmap;
	int irq_cnt[MT8196_MEMIF_NUM];
	int stf_positive_gain_db;
	int dram_resource_counter;
	int sgen_sel;
	int sgen_mode;
	int sgen_rate;
	int sgen_amplitude;
	/* xrun assert */
	int xrun_assert[MT8196_MEMIF_NUM];

	/* dai */
	bool dai_on[MT8196_DAI_NUM];
	void *dai_priv[MT8196_DAI_NUM];

	/* adda */
	int mtkaif_protocol;
	int mtkaif_chosen_phase[4];
	int mtkaif_phase_cycle[4];
	int mtkaif_calibration_num_phase;
	int mtkaif_dmic;
	int mtkaif_dmic_ch34;
	int mtkaif_adda6_only;
	/* support ap_dmic */
	int ap_dmic;
	unsigned int audio_r_miso1_enable;
	unsigned int miso_only;

	/* add for vs1 voter */
	/* adda dl/ul is on */
	bool is_adda_dl_on;
	bool is_adda_ul_on;
	/* vow is on */
	bool is_vow_enable;
	/* adda dl vol idx is at maximum */
	bool is_adda_dl_max_vol;
	/* current vote status of vs1 */
	bool is_mt6363_vote;

	/* mck */
	int mck_rate[MT8196_MCK_NUM];

	/* channel merge */
	unsigned int cm_rate[CM_NUM];
};

int mt8196_dai_adda_register(struct mtk_base_afe *afe);
int mt8196_dai_i2s_register(struct mtk_base_afe *afe);
int mt8196_dai_tdm_register(struct mtk_base_afe *afe);

unsigned int mt8196_general_rate_transform(struct device *dev,
		unsigned int rate);
unsigned int mt8196_rate_transform(struct device *dev,
				   unsigned int rate, int aud_blk);
int mt8196_dai_set_priv(struct mtk_base_afe *afe, int id,
			int priv_size, const void *priv_data);
#endif
