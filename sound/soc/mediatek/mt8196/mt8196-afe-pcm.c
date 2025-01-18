// SPDX-License-Identifier: GPL-2.0
/*
 *  Mediatek ALSA SoC AFE platform driver for 8196
 *
 *  Copyright (c) 2024 MediaTek Inc.
 *  Author: Darren Ye <darren.ye@mediatek.com>
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <sound/soc.h>
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <linux/arm-smccc.h> /* for Kernel Native SMC API */
#include <linux/of_reserved_mem.h>
#include <linux/soc/mediatek/mtk_sip_svc.h> /* for SMC ID table */

#include "mt8196-afe-common.h"
#include "mtk-afe-platform-driver.h"
#include "mtk-afe-fe-dai.h"
#include "mt8196-afe-clk.h"
#include "mt8196-afe-gpio.h"
#include "mt8196-interconnection.h"
/* FORCE_FPGA_ENABLE_IRQ use irq in fpga */
#define FORCE_FPGA_ENABLE_IRQ

static const struct snd_pcm_hardware mt8196_afe_hardware = {
	.info = (SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_NO_PERIOD_WAKEUP |
		 SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_MMAP_VALID),
	.formats = (SNDRV_PCM_FMTBIT_S16_LE |
		    SNDRV_PCM_FMTBIT_S24_LE |
		    SNDRV_PCM_FMTBIT_S32_LE),
	.period_bytes_min = 96,
	.period_bytes_max = 4 * 48 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.buffer_bytes_max = 256 * 1024,
	.fifo_size = 0,
};

static int mt8196_fe_startup(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int memif_num = cpu_dai->id;
	struct mtk_base_afe_memif *memif = &afe->memif[memif_num];
	const struct snd_pcm_hardware *mtk_afe_hardware = afe->mtk_afe_hardware;
	int ret;

	dev_dbg(afe->dev, "%s(), memif_num: %d.\n", __func__, memif_num);

	memif->substream = substream;

	snd_pcm_hw_constraint_step(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 16);

	snd_soc_set_runtime_hwparams(substream, mtk_afe_hardware);

	ret = snd_pcm_hw_constraint_integer(runtime,
						SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		dev_info(afe->dev, "snd_pcm_hw_constraint_integer failed\n");

	/* dynamic allocate irq to memif */
	if (memif->irq_usage < 0) {
		int irq_id = mtk_dynamic_irq_acquire(afe);

		if (irq_id != afe->irqs_size) {
			/* link */
			memif->irq_usage = irq_id;
		} else {
			dev_info(afe->dev, "%s() error: no more asys irq\n",
				__func__);
			ret = -EBUSY;
		}
	}
	return ret;
}

void mt8196_fe_shutdown(struct snd_pcm_substream *substream,
			struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int memif_num = cpu_dai->id;
	struct mtk_base_afe_memif *memif = &afe->memif[memif_num];
	int irq_id = memif->irq_usage;

	dev_dbg(afe->dev, "%s(), memif_num: %d.\n", __func__, memif_num);

	memif->substream = NULL;
	afe_priv->irq_cnt[memif_num] = 0;
	afe_priv->xrun_assert[memif_num] = 0;

	if (!memif->const_irq) {
		mtk_dynamic_irq_release(afe, irq_id);
		memif->irq_usage = -1;
		memif->substream = NULL;
	}
}

int mt8196_fe_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_pcm_runtime *const runtime = substream->runtime;
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int id = cpu_dai->id;
	struct mtk_base_afe_memif *memif = &afe->memif[id];
	int irq_id = memif->irq_usage;
	struct mtk_base_afe_irq *irqs = &afe->irqs[irq_id];
	const struct mtk_base_irq_data *irq_data = irqs->irq_data;
	unsigned int counter = runtime->period_size;
	unsigned int rate = runtime->rate;
	int fs;
	int ret = 0;
	unsigned int tmp_reg = 0;

	dev_info(afe->dev, "%s(), %s cmd %d, irq_id %d\n", __func__,
		memif->data->name, cmd, irq_id);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		dev_info(afe->dev, "%s(), %s cmd %d, id %d\n", __func__,
			memif->data->name, cmd, id);
		ret = mtk_memif_set_enable(afe, id);
		if (ret) {
			dev_err(afe->dev, "%s(), error, id %d, memif enable, ret %d\n",
				__func__, id, ret);
			return ret;
		}

		/*
		 * for small latency record
		 * ul memif need read some data before irq enable
		 */
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
			if ((runtime->period_size * 1000) / rate <= 10)
				udelay(300);
		}

		/* set irq counter */
		if (afe_priv->irq_cnt[id] > 0)
			counter = afe_priv->irq_cnt[id];

		mtk_regmap_update_bits(afe->regmap, irq_data->irq_cnt_reg,
				   irq_data->irq_cnt_maskbit,
				   counter, irq_data->irq_cnt_shift);

		/* set irq fs */
		fs = afe->irq_fs(substream, runtime->rate);
		if (fs < 0)
			return -EINVAL;

		if (irq_data->irq_fs_reg >= 0)
			mtk_regmap_update_bits(afe->regmap, irq_data->irq_fs_reg,
				   irq_data->irq_fs_maskbit,
				   fs, irq_data->irq_fs_shift);

		/* enable interrupt */
		mtk_regmap_update_bits(afe->regmap, irq_data->irq_en_reg,
				       1, 1, irq_data->irq_en_shift);

		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ret = mtk_memif_set_disable(afe, id);
		if (ret) {
			dev_info(afe->dev,
				"%s(), error, id %d, memif enable, ret %d\n",
				__func__, id, ret);
		}

		/* disable interrupt */
		mtk_regmap_update_bits(afe->regmap, irq_data->irq_en_reg,
			       1, 0, irq_data->irq_en_shift);

		/* clear pending IRQ */
		regmap_read(afe->regmap, irq_data->irq_clr_reg, &tmp_reg);
		regmap_update_bits(afe->regmap, irq_data->irq_clr_reg,
			   AFE_IRQ_CLR_CFG_MASK_SFT | AFE_IRQ_MISS_FLAG_CLR_CFG_MASK_SFT,
			   tmp_reg^(AFE_IRQ_CLR_CFG_MASK_SFT | AFE_IRQ_MISS_FLAG_CLR_CFG_MASK_SFT));

		return ret;
	default:
		return -EINVAL;
	}
}

static int mt8196_memif_fs(struct snd_pcm_substream *substream,
			   unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = NULL;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int id = cpu_dai->id;
	unsigned int rate_reg = 0;
	int cm = 0;

	if (!component)
		return -EINVAL;

	afe = snd_soc_component_get_drvdata(component);

	if (!afe)
		return -EINVAL;

	rate_reg = mt8196_rate_transform(afe->dev, rate, id);

	switch (id) {
	case MT8196_MEMIF_VUL8:
	case MT8196_MEMIF_VUL_CM0:
		cm = CM0;
		break;
	case MT8196_MEMIF_VUL9:
	case MT8196_MEMIF_VUL_CM1:
		cm = CM1;
		break;
	case MT8196_MEMIF_VUL10:
	case MT8196_MEMIF_VUL_CM2:
		cm = CM2;
		break;
	default:
		cm = CM0;
		break;
	}

	mt8196_set_cm_rate(afe, cm, rate_reg);

	return rate_reg;
}

static int mt8196_get_dai_fs(struct mtk_base_afe *afe,
			     int dai_id, unsigned int rate)
{
	return mt8196_rate_transform(afe->dev, rate, dai_id);
}

static int mt8196_irq_fs(struct snd_pcm_substream *substream, unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = NULL;

	if (!component)
		return -EINVAL;
	afe = snd_soc_component_get_drvdata(component);
	return mt8196_general_rate_transform(afe->dev, rate);
}

int mt8196_get_memif_pbuf_size(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	if ((runtime->period_size * 1000) / runtime->rate > 10)
		return MT8196_MEMIF_PBUF_SIZE_256_BYTES;
	else
		return MT8196_MEMIF_PBUF_SIZE_32_BYTES;
}

/* FE DAIs */
static const struct snd_soc_dai_ops mt8196_memif_dai_ops = {
	.startup        = mt8196_fe_startup,
	.shutdown       = mt8196_fe_shutdown,
	.hw_params      = mtk_afe_fe_hw_params,
	.hw_free        = mtk_afe_fe_hw_free,
	.prepare        = mtk_afe_fe_prepare,
	.trigger        = mt8196_fe_trigger,
};

#define MTK_PCM_RATES (SNDRV_PCM_RATE_8000_48000 |\
		       SNDRV_PCM_RATE_88200 |\
		       SNDRV_PCM_RATE_96000 |\
		       SNDRV_PCM_RATE_176400 |\
		       SNDRV_PCM_RATE_192000)

#define MTK_PCM_DAI_RATES (SNDRV_PCM_RATE_8000 |\
			   SNDRV_PCM_RATE_16000 |\
			   SNDRV_PCM_RATE_32000 |\
			   SNDRV_PCM_RATE_48000)

#define MTK_PCM_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mt8196_memif_dai_driver[] = {
	/* FE DAIs: memory intefaces to CPU */
	{
		.name = "DL0",
		.id = MT8196_MEMIF_DL0,
		.playback = {
			.stream_name = "DL0",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL1",
		.id = MT8196_MEMIF_DL1,
		.playback = {
			.stream_name = "DL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL2",
		.id = MT8196_MEMIF_DL2,
		.playback = {
			.stream_name = "DL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL3",
		.id = MT8196_MEMIF_DL3,
		.playback = {
			.stream_name = "DL3",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL4",
		.id = MT8196_MEMIF_DL4,
		.playback = {
			.stream_name = "DL4",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL5",
		.id = MT8196_MEMIF_DL5,
		.playback = {
			.stream_name = "DL5",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL6",
		.id = MT8196_MEMIF_DL6,
		.playback = {
			.stream_name = "DL6",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL7",
		.id = MT8196_MEMIF_DL7,
		.playback = {
			.stream_name = "DL7",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL8",
		.id = MT8196_MEMIF_DL8,
		.playback = {
			.stream_name = "DL8",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL23",
		.id = MT8196_MEMIF_DL23,
		.playback = {
			.stream_name = "DL23",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL24",
		.id = MT8196_MEMIF_DL24,
		.playback = {
			.stream_name = "DL24",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL25",
		.id = MT8196_MEMIF_DL25,
		.playback = {
			.stream_name = "DL25",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL26",
		.id = MT8196_MEMIF_DL26,
		.playback = {
			.stream_name = "DL26",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL_4CH",
		.id = MT8196_MEMIF_DL_4CH,
		.playback = {
			.stream_name = "DL_4CH",
			.channels_min = 1,
			.channels_max = 4,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "DL_24CH",
		.id = MT8196_MEMIF_DL_24CH,
		.playback = {
			.stream_name = "DL_24CH",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL0",
		.id = MT8196_MEMIF_VUL0,
		.capture = {
			.stream_name = "UL0",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL1",
		.id = MT8196_MEMIF_VUL1,
		.capture = {
			.stream_name = "UL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL2",
		.id = MT8196_MEMIF_VUL2,
		.capture = {
			.stream_name = "UL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL3",
		.id = MT8196_MEMIF_VUL3,
		.capture = {
			.stream_name = "UL3",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL4",
		.id = MT8196_MEMIF_VUL4,
		.capture = {
			.stream_name = "UL4",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL5",
		.id = MT8196_MEMIF_VUL5,
		.capture = {
			.stream_name = "UL5",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL6",
		.id = MT8196_MEMIF_VUL6,
		.capture = {
			.stream_name = "UL6",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL7",
		.id = MT8196_MEMIF_VUL7,
		.capture = {
			.stream_name = "UL7",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL8",
		.id = MT8196_MEMIF_VUL8,
		.capture = {
			.stream_name = "UL8",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL9",
		.id = MT8196_MEMIF_VUL9,
		.capture = {
			.stream_name = "UL9",
			.channels_min = 1,
			.channels_max = 16,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL10",
		.id = MT8196_MEMIF_VUL10,
		.capture = {
			.stream_name = "UL10",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL24",
		.id = MT8196_MEMIF_VUL24,
		.capture = {
			.stream_name = "UL24",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL25",
		.id = MT8196_MEMIF_VUL25,
		.capture = {
			.stream_name = "UL25",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL26",
		.id = MT8196_MEMIF_VUL26,
		.capture = {
			.stream_name = "UL26",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_CM0",
		.id = MT8196_MEMIF_VUL_CM0,
		.capture = {
			.stream_name = "UL_CM0",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_CM1",
		.id = MT8196_MEMIF_VUL_CM1,
		.capture = {
			.stream_name = "UL_CM1",
			.channels_min = 1,
			.channels_max = 16,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_CM2",
		.id = MT8196_MEMIF_VUL_CM2,
		.capture = {
			.stream_name = "UL_CM2",
			.channels_min = 1,
			.channels_max = 32,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_ETDM_IN0",
		.id = MT8196_MEMIF_ETDM_IN0,
		.capture = {
			.stream_name = "UL_ETDM_IN0",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_ETDM_IN1",
		.id = MT8196_MEMIF_ETDM_IN1,
		.capture = {
			.stream_name = "UL_ETDM_IN1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_ETDM_IN2",
		.id = MT8196_MEMIF_ETDM_IN2,
		.capture = {
			.stream_name = "UL_ETDM_IN2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_ETDM_IN3",
		.id = MT8196_MEMIF_ETDM_IN3,
		.capture = {
			.stream_name = "UL_ETDM_IN3",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_ETDM_IN4",
		.id = MT8196_MEMIF_ETDM_IN4,
		.capture = {
			.stream_name = "UL_ETDM_IN4",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "UL_ETDM_IN6",
		.id = MT8196_MEMIF_ETDM_IN6,
		.capture = {
			.stream_name = "UL_ETDM_IN6",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
	{
		.name = "HDMI",
		.id = MT8196_MEMIF_HDMI,
		.playback = {
			.stream_name = "HDMI",
			.channels_min = 2,
			.channels_max = 8,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mt8196_memif_dai_ops,
	},
};

static const struct snd_kcontrol_new mt8196_pcm_kcontrols[] = {
};

enum {
	CM0_MUX_VUL8_2CH,
	CM0_MUX_VUL8_8CH,
	CM0_MUX_MASK,
};
enum {
	CM1_MUX_VUL9_2CH,
	CM1_MUX_VUL9_16CH,
	CM1_MUX_MASK,
};
enum {
	CM2_MUX_VUL10_2CH,
	CM2_MUX_VUL10_32CH,
	CM2_MUX_MASK,
};

static int ul_cm0_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol,
			int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt);
	unsigned int channels = 0;

	dev_dbg(afe->dev, "%s(), event 0x%x, name %s\n",
		 __func__, event, w->name);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		channels = 3;
		mt8196_enable_cm_bypass(afe, CM0, 0x0);
		mt8196_set_cm(afe, CM0, true, false, channels);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		mt8196_enable_cm_bypass(afe, CM0, 0x1);
		break;
	default:
		break;
	}
	return 0;
}

static int ul_cm1_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol,
			int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt);
	unsigned int channels = 0;

	dev_info(afe->dev, "%s(), event 0x%x, name %s\n",
		 __func__, event, w->name);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		channels = 4;
		mt8196_enable_cm_bypass(afe, CM1, 0x0);
		mt8196_set_cm(afe, CM1, true, false, channels);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		mt8196_enable_cm_bypass(afe, CM1, 0x1);
		break;
	default:
		break;
	}
	return 0;
}

static int ul_cm2_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol,
			int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt);
	unsigned int channels = 0;

	dev_info(afe->dev, "%s(), event 0x%x, name %s\n",
		 __func__, event, w->name);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		channels = 4;
		mt8196_enable_cm_bypass(afe, CM2, 0x0);
		mt8196_set_cm(afe, CM2, true, false, channels);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		mt8196_enable_cm_bypass(afe, CM2, 0x1);
		break;
	default:
		break;
	}
	return 0;
}

/* dma widget & routes*/
static const struct snd_kcontrol_new memif_ul0_ch1_mix[] = {
	/* Normal record */
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN018_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN018_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN018_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN018_0,
	I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN018_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN018_0,
				    I_ADDA_UL_CH6, 1, 0),
	/* FM */
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH1", AFE_CONN018_0,
				    I_CONNSYS_I2S_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN018_1,
				    I_DL0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN018_1,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN018_1,
				    I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN018_1,
				    I_DL3_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL4_CH1", AFE_CONN018_1,
				    I_DL4_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH1", AFE_CONN018_1,
				    I_DL6_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL7_CH1", AFE_CONN018_1,
				    I_DL7_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL23_CH1", AFE_CONN018_2,
				    I_DL23_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH1", AFE_CONN018_1,
				    I_DL_24CH_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN018_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN018_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH1", AFE_CONN018_4,
				    I_I2SIN0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN1_CH1", AFE_CONN018_4,
				    I_I2SIN1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH1", AFE_CONN018_6,
				    I_SRC_0_OUT_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_2_OUT_CH1", AFE_CONN018_6,
				    I_SRC_2_OUT_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul0_ch2_mix[] = {
	/* Normal record */
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN019_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN019_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN019_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN019_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN019_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN019_0,
				    I_ADDA_UL_CH6, 1, 0),
	/* FM */
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH2", AFE_CONN019_0,
				    I_CONNSYS_I2S_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH2", AFE_CONN019_1,
				    I_DL0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN019_1,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN019_1,
				    I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN019_1,
				    I_DL3_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL4_CH2", AFE_CONN019_1,
				    I_DL4_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH2", AFE_CONN019_1,
				    I_DL6_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL7_CH2", AFE_CONN019_1,
				    I_DL7_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL23_CH2", AFE_CONN019_2,
				    I_DL23_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH2", AFE_CONN019_1,
				    I_DL_24CH_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN019_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH2", AFE_CONN019_4,
				    I_PCM_0_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN019_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH2", AFE_CONN019_4,
				    I_PCM_1_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH2", AFE_CONN019_4,
				    I_I2SIN0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN1_CH2", AFE_CONN019_4,
				    I_I2SIN1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH2", AFE_CONN019_6,
				    I_SRC_0_OUT_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_2_OUT_CH2", AFE_CONN019_6,
				    I_SRC_2_OUT_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul1_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN020_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH1", AFE_CONN020_0,
				    I_CONNSYS_I2S_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN020_1,
				    I_DL0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN020_1,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN020_1,
				    I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN020_1,
				    I_DL3_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL4_CH1", AFE_CONN020_1,
				    I_DL4_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH1", AFE_CONN020_1,
				    I_DL6_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL7_CH1", AFE_CONN020_1,
				    I_DL7_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL23_CH1", AFE_CONN020_2,
				    I_DL23_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH1", AFE_CONN020_1,
				    I_DL_24CH_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN020_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN020_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH1", AFE_CONN020_4,
				    I_I2SIN0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN1_CH1", AFE_CONN020_4,
				    I_I2SIN1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN3_CH1", AFE_CONN020_4,
				    I_I2SIN3_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN4_CH1", AFE_CONN020_4,
				    I_I2SIN4_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH1", AFE_CONN020_5,
					I_I2SIN6_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH1", AFE_CONN020_6,
				    I_SRC_0_OUT_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_2_OUT_CH1", AFE_CONN020_6,
				    I_SRC_2_OUT_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul1_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN021_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH2", AFE_CONN021_0,
				    I_CONNSYS_I2S_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH2", AFE_CONN021_1,
				    I_DL0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN021_1,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN021_1,
				    I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN021_1,
				    I_DL3_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL4_CH2", AFE_CONN021_1,
				    I_DL4_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH2", AFE_CONN021_1,
				    I_DL6_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL7_CH2", AFE_CONN021_1,
				    I_DL7_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL23_CH2", AFE_CONN021_2,
				    I_DL23_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH2", AFE_CONN021_1,
				    I_DL_24CH_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN021_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH2", AFE_CONN021_4,
				    I_PCM_0_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN021_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH2", AFE_CONN021_4,
				    I_PCM_1_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH2", AFE_CONN021_4,
				    I_I2SIN0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN1_CH2", AFE_CONN021_4,
				    I_I2SIN1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN3_CH2", AFE_CONN021_4,
				    I_I2SIN3_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN4_CH2", AFE_CONN021_4,
				    I_I2SIN4_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH2", AFE_CONN021_5,
					I_I2SIN6_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH2", AFE_CONN021_6,
				    I_SRC_0_OUT_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_2_OUT_CH2", AFE_CONN021_6,
				    I_SRC_2_OUT_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN022_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN022_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN022_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN022_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_GAIN1_OUT_CH1", AFE_CONN022_0,
				    I_GAIN1_OUT_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_1_OUT_CH1", AFE_CONN022_6,
				    I_SRC_1_OUT_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN023_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN023_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN023_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN023_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_GAIN1_OUT_CH2", AFE_CONN023_0,
				    I_GAIN1_OUT_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_1_OUT_CH2", AFE_CONN023_6,
				    I_SRC_1_OUT_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul3_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN024_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH1", AFE_CONN024_4,
				    I_I2SIN0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN1_CH1", AFE_CONN024_4,
				    I_I2SIN1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN3_CH1", AFE_CONN024_4,
				    I_I2SIN3_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN4_CH1", AFE_CONN024_4,
				    I_I2SIN4_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul3_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN025_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH2", AFE_CONN025_4,
				    I_I2SIN0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN1_CH2", AFE_CONN025_4,
				    I_I2SIN1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN3_CH2", AFE_CONN025_4,
				    I_I2SIN3_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN4_CH2", AFE_CONN025_4,
				    I_I2SIN4_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul4_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN026_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN026_1,
				    I_DL0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN026_1,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH1", AFE_CONN026_1,
				    I_DL6_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN026_1,
				    I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN026_1,
				    I_DL3_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH1", AFE_CONN026_1,
				    I_DL_24CH_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN026_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN026_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH1", AFE_CONN026_4,
					I_I2SIN0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_GAIN0_OUT_CH1", AFE_CONN026_0,
				    I_GAIN0_OUT_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul4_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN027_0,
					I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH2", AFE_CONN027_1,
				    I_DL0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN027_1,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH2", AFE_CONN027_1,
				    I_DL6_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN027_1,
				    I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN027_1,
				    I_DL3_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH2", AFE_CONN027_1,
				    I_DL_24CH_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN027_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH2", AFE_CONN027_4,
				    I_PCM_0_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN027_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH2", AFE_CONN027_4,
				    I_PCM_1_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH2", AFE_CONN027_4,
					I_I2SIN0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_GAIN0_OUT_CH2", AFE_CONN027_0,
				    I_GAIN0_OUT_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul5_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN028_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN028_1,
				    I_DL0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN028_1,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH1", AFE_CONN028_1,
				    I_DL6_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN028_1,
				    I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN028_1,
				    I_DL3_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH1", AFE_CONN028_1,
				    I_DL_24CH_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN028_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN028_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("GAIN0_OUT_CH1", AFE_CONN028_0,
				    I_GAIN0_OUT_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN3_CH1", AFE_CONN028_4,
				    I_I2SIN3_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul5_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN029_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH2", AFE_CONN029_1,
				    I_DL0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN029_1,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH2", AFE_CONN029_1,
				    I_DL6_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN029_1,
				    I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN029_1,
				    I_DL3_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH2", AFE_CONN029_1,
				    I_DL_24CH_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN029_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH2", AFE_CONN029_4,
				    I_PCM_0_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN029_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH2", AFE_CONN029_4,
				    I_PCM_1_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("GAIN0_OUT_CH2", AFE_CONN029_0,
				    I_GAIN0_OUT_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN3_CH2", AFE_CONN029_4,
				    I_I2SIN3_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul6_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN030_0,
					I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH1", AFE_CONN030_0,
				    I_CONNSYS_I2S_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN030_1,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN030_1,
				    I_DL2_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul6_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN031_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH2", AFE_CONN031_0,
				    I_CONNSYS_I2S_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH2", AFE_CONN031_1,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN031_1,
				    I_DL2_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul7_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN032_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN032_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN032_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN032_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH1", AFE_CONN032_0,
				    I_CONNSYS_I2S_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN032_1,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN032_1,
				    I_DL2_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul7_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN033_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN033_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN033_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN033_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("CONNSYS_I2S_CH2", AFE_CONN033_0,
				    I_CONNSYS_I2S_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH2", AFE_CONN033_1,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN033_1,
				    I_DL2_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul8_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN034_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN034_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN034_4,
				    I_PCM_1_CAP_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul8_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN035_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH1", AFE_CONN035_4,
				    I_PCM_0_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_0_CAP_CH2", AFE_CONN035_4,
				    I_PCM_0_CAP_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN035_4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH2", AFE_CONN035_4,
				    I_PCM_1_CAP_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul9_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN036_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN036_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN036_0,
				    I_ADDA_UL_CH3, 1, 0),
};

static const struct snd_kcontrol_new memif_ul9_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN037_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN037_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN037_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN037_0,
				    I_ADDA_UL_CH4, 1, 0),
};

static const struct snd_kcontrol_new memif_ul10_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN038_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN038_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN038_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN038_0,
				    I_ADDA_UL_CH4, 1, 0),
};

static const struct snd_kcontrol_new memif_ul10_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN039_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN039_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN039_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN039_0,
				    I_ADDA_UL_CH4, 1, 0),
};

static const struct snd_kcontrol_new memif_ul24_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN066_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH1", AFE_CONN066_4,
					I_I2SIN0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH1", AFE_CONN066_5,
					I_I2SIN6_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul24_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN067_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH2", AFE_CONN067_4,
					I_I2SIN0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH2", AFE_CONN067_5,
					I_I2SIN6_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul25_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH1", AFE_CONN068_4,
					I_I2SIN0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH1", AFE_CONN068_5,
					I_I2SIN6_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul25_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH2", AFE_CONN069_4,
					I_I2SIN0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH2", AFE_CONN069_5,
					I_I2SIN6_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul26_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH1", AFE_CONN070_4,
					I_I2SIN0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH1", AFE_CONN070_5,
					I_I2SIN6_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul26_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN0_CH2", AFE_CONN071_4,
					I_I2SIN0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("I2SIN6_CH2", AFE_CONN071_5,
					I_I2SIN6_CH2, 1, 0),
};

static const struct snd_kcontrol_new mtk_dsp_dl_playback_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DSP_DL0", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DSP_DL2", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DSP_DL1", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DSP_DL6", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DSP_DL3", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DSP_DL_24CH", SND_SOC_NOPM, 0, 1, 0),
};

static const struct snd_kcontrol_new memif_ul_cm0_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN040_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN040_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN040_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN040_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_GAIN1_OUT_CH1", AFE_CONN040_0,
				    I_GAIN1_OUT_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH1", AFE_CONN040_6,
				    I_SRC_0_OUT_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_1_OUT_CH1", AFE_CONN040_6,
				    I_SRC_1_OUT_CH1, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm0_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN041_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN041_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN041_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN041_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_GAIN1_OUT_CH2", AFE_CONN041_0,
				    I_GAIN1_OUT_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH2", AFE_CONN041_6,
				    I_SRC_0_OUT_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_1_OUT_CH2", AFE_CONN041_6,
				    I_SRC_1_OUT_CH2, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm0_ch3_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN042_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN042_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN042_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN042_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm0_ch4_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN043_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN043_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN043_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN043_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm0_ch5_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN044_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN044_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN044_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN044_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm0_ch6_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN045_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN045_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN045_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN045_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm0_ch7_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN046_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN046_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN046_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN046_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm0_ch8_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN047_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN047_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN047_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN047_0,
				    I_ADDA_UL_CH4, 1, 0),
};

static const struct snd_kcontrol_new memif_ul_cm1_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN048_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN048_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN048_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN048_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN048_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN048_0,
				    I_ADDA_UL_CH6, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH1", AFE_CONN048_6,
				    I_SRC_0_OUT_CH1, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN049_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN049_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN049_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN049_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN049_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN049_0,
				    I_ADDA_UL_CH6, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH2", AFE_CONN049_6,
				    I_SRC_0_OUT_CH2, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch3_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN050_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN050_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN050_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN050_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN050_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN050_0,
				    I_ADDA_UL_CH6, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch4_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN051_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN051_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN051_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN051_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN051_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN051_0,
				    I_ADDA_UL_CH6, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch5_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN052_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN052_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN052_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN052_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN052_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN052_0,
				    I_ADDA_UL_CH6, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch6_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN053_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN053_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN053_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN053_0,
				    I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN053_0,
				    I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN053_0,
				    I_ADDA_UL_CH6, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch7_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN054_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN054_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN054_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN054_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch8_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN055_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN055_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN055_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN055_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch9_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN056_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN056_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN056_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN056_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch10_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN057_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN057_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN057_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN057_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch11_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN058_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN058_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN058_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN058_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch12_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN059_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN059_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN059_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN059_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch13_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN060_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN060_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN060_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN060_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch14_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN061_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN061_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN061_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN061_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch15_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN062_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN062_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN062_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN062_0,
				    I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm1_ch16_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN063_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN063_0,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN063_0,
				    I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN063_0,
				    I_ADDA_UL_CH4, 1, 0),
};

static const struct snd_kcontrol_new memif_ul_cm2_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN064_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN064_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN064_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN064_0, I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN064_0, I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN064_0, I_ADDA_UL_CH6, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH1", AFE_CONN064_6,
				    I_SRC_0_OUT_CH1, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN065_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN065_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN065_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN065_0, I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN065_0, I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN065_0, I_ADDA_UL_CH6, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("HW_SRC_0_OUT_CH2", AFE_CONN065_6,
				    I_SRC_0_OUT_CH2, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch3_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN066_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN066_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN066_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN066_0, I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN066_0, I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN066_0, I_ADDA_UL_CH6, 1, 0)
};
static const struct snd_kcontrol_new memif_ul_cm2_ch4_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN067_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN067_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN067_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN067_0, I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN067_0, I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN067_0, I_ADDA_UL_CH6, 1, 0)
};
static const struct snd_kcontrol_new memif_ul_cm2_ch5_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN068_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN068_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN068_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN068_0, I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN068_0, I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN068_0, I_ADDA_UL_CH6, 1, 0)
};
static const struct snd_kcontrol_new memif_ul_cm2_ch6_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN069_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN069_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN069_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN069_0, I_ADDA_UL_CH4, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH5", AFE_CONN069_0, I_ADDA_UL_CH5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH6", AFE_CONN069_0, I_ADDA_UL_CH6, 1, 0)
};
static const struct snd_kcontrol_new memif_ul_cm2_ch7_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN070_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN070_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN070_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN070_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch8_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN071_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN071_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN071_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN071_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch9_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN072_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN072_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN072_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN072_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch10_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN073_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN073_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN073_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN073_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch11_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN074_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN074_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN074_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN074_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch12_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN075_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN075_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN075_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN075_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch13_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN076_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN076_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN076_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN076_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch14_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN077_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN077_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN077_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN077_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch15_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN078_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN078_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN078_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN078_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch16_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN079_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN079_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN079_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN079_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch17_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN080_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN080_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN080_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN080_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch18_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN081_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN081_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN081_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN081_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch19_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN082_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN082_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN082_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN082_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch20_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN083_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN083_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN083_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN083_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch21_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN084_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN084_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN084_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN084_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch22_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN085_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN085_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN085_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN085_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch23_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN086_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN086_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN086_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN086_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch24_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN087_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN087_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN087_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN087_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch25_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN088_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN088_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN088_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN088_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch26_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN089_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN089_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN089_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN089_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch27_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN090_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN090_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN090_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN090_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch28_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN091_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN091_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN091_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN091_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch29_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN092_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN092_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN092_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN092_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch30_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN093_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN093_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN093_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN093_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch31_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN094_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN094_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN094_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN094_0, I_ADDA_UL_CH4, 1, 0),
};
static const struct snd_kcontrol_new memif_ul_cm2_ch32_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN095_0, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN095_0, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH3", AFE_CONN095_0, I_ADDA_UL_CH3, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH4", AFE_CONN095_0, I_ADDA_UL_CH4, 1, 0),
};

static const char * const cm0_mux_map[] = {
	"CM0_8CH_PATH",
	"CM0_2CH_PATH",
};
static const char * const cm1_mux_map[] = {
	"CM1_16CH_PATH",
	"CM1_2CH_PATH",
};
static const char * const cm2_mux_map[] = {
	"CM2_32CH_PATH",
	"CM2_2CH_PATH",
};

static int cm0_mux_map_value[] = {
	CM0_MUX_VUL8_8CH,
	CM0_MUX_VUL8_2CH,
};
static int cm1_mux_map_value[] = {
	CM1_MUX_VUL9_16CH,
	CM1_MUX_VUL9_2CH,
};
static int cm2_mux_map_value[] = {
	CM2_MUX_VUL10_32CH,
	CM2_MUX_VUL10_2CH,
};

static SOC_VALUE_ENUM_SINGLE_DECL(ul_cm0_mux_map_enum,
				  AFE_CM0_CON0,
				  AFE_CM0_OUTPUT_MUX_SFT,
				  AFE_CM0_OUTPUT_MUX_MASK,
				  cm0_mux_map,
				  cm0_mux_map_value);
static SOC_VALUE_ENUM_SINGLE_DECL(ul_cm1_mux_map_enum,
				  AFE_CM1_CON0,
				  AFE_CM1_OUTPUT_MUX_SFT,
				  AFE_CM1_OUTPUT_MUX_MASK,
				  cm1_mux_map,
				  cm1_mux_map_value);
static SOC_VALUE_ENUM_SINGLE_DECL(ul_cm2_mux_map_enum,
				  AFE_CM2_CON0,
				  AFE_CM2_OUTPUT_MUX_SFT,
				  AFE_CM2_OUTPUT_MUX_MASK,
				  cm2_mux_map,
				  cm2_mux_map_value);

static const struct snd_kcontrol_new ul_cm0_mux_control =
	SOC_DAPM_ENUM("CM0_UL_MUX Select", ul_cm0_mux_map_enum);
static const struct snd_kcontrol_new ul_cm1_mux_control =
	SOC_DAPM_ENUM("CM1_UL_MUX Select", ul_cm1_mux_map_enum);
static const struct snd_kcontrol_new ul_cm2_mux_control =
	SOC_DAPM_ENUM("CM2_UL_MUX Select", ul_cm2_mux_map_enum);

static const struct snd_soc_dapm_widget mt8196_memif_widgets[] = {
	/* inter-connections */
	SND_SOC_DAPM_MIXER("UL0_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul0_ch1_mix, ARRAY_SIZE(memif_ul0_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL0_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul0_ch2_mix, ARRAY_SIZE(memif_ul0_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch1_mix, ARRAY_SIZE(memif_ul1_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL1_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch2_mix, ARRAY_SIZE(memif_ul1_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch1_mix, ARRAY_SIZE(memif_ul2_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL2_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch2_mix, ARRAY_SIZE(memif_ul2_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL3_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul3_ch1_mix, ARRAY_SIZE(memif_ul3_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL3_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul3_ch2_mix, ARRAY_SIZE(memif_ul3_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL4_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul4_ch1_mix, ARRAY_SIZE(memif_ul4_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL4_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul4_ch2_mix, ARRAY_SIZE(memif_ul4_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL5_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul5_ch1_mix, ARRAY_SIZE(memif_ul5_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL5_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul5_ch2_mix, ARRAY_SIZE(memif_ul5_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL6_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul6_ch1_mix, ARRAY_SIZE(memif_ul6_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL6_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul6_ch2_mix, ARRAY_SIZE(memif_ul6_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL7_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul7_ch1_mix, ARRAY_SIZE(memif_ul7_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL7_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul7_ch2_mix, ARRAY_SIZE(memif_ul7_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL8_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul8_ch1_mix, ARRAY_SIZE(memif_ul8_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL8_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul8_ch2_mix, ARRAY_SIZE(memif_ul8_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL9_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul9_ch1_mix, ARRAY_SIZE(memif_ul9_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL9_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul9_ch2_mix, ARRAY_SIZE(memif_ul9_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL10_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul10_ch1_mix, ARRAY_SIZE(memif_ul10_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL10_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul10_ch2_mix, ARRAY_SIZE(memif_ul10_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL24_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul24_ch1_mix, ARRAY_SIZE(memif_ul24_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL24_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul24_ch2_mix, ARRAY_SIZE(memif_ul24_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL25_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul25_ch1_mix, ARRAY_SIZE(memif_ul25_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL25_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul25_ch2_mix, ARRAY_SIZE(memif_ul25_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL26_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul26_ch1_mix, ARRAY_SIZE(memif_ul26_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL26_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul26_ch2_mix, ARRAY_SIZE(memif_ul26_ch2_mix)),

	SND_SOC_DAPM_MIXER("DSP_DL", SND_SOC_NOPM, 0, 0,
			   mtk_dsp_dl_playback_mix,
			   ARRAY_SIZE(mtk_dsp_dl_playback_mix)),

	SND_SOC_DAPM_MIXER("UL_CM0_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch1_mix, ARRAY_SIZE(memif_ul_cm0_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL_CM0_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch2_mix, ARRAY_SIZE(memif_ul_cm0_ch2_mix)),
	SND_SOC_DAPM_MIXER("UL_CM0_CH3", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch3_mix, ARRAY_SIZE(memif_ul_cm0_ch3_mix)),
	SND_SOC_DAPM_MIXER("UL_CM0_CH4", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch4_mix, ARRAY_SIZE(memif_ul_cm0_ch4_mix)),
	SND_SOC_DAPM_MIXER("UL_CM0_CH5", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch5_mix, ARRAY_SIZE(memif_ul_cm0_ch5_mix)),
	SND_SOC_DAPM_MIXER("UL_CM0_CH6", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch6_mix, ARRAY_SIZE(memif_ul_cm0_ch6_mix)),
	SND_SOC_DAPM_MIXER("UL_CM0_CH7", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch7_mix, ARRAY_SIZE(memif_ul_cm0_ch7_mix)),
	SND_SOC_DAPM_MIXER("UL_CM0_CH8", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm0_ch8_mix, ARRAY_SIZE(memif_ul_cm0_ch8_mix)),
	SND_SOC_DAPM_MUX_E("CM0_UL_MUX", SND_SOC_NOPM, 0, 0,
			   &ul_cm0_mux_control,
			   ul_cm0_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_MIXER("UL_CM1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch1_mix, ARRAY_SIZE(memif_ul_cm1_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch2_mix, ARRAY_SIZE(memif_ul_cm1_ch2_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH3", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch3_mix, ARRAY_SIZE(memif_ul_cm1_ch3_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH4", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch4_mix, ARRAY_SIZE(memif_ul_cm1_ch4_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH5", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch5_mix, ARRAY_SIZE(memif_ul_cm1_ch5_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH6", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch6_mix, ARRAY_SIZE(memif_ul_cm1_ch6_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH7", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch7_mix, ARRAY_SIZE(memif_ul_cm1_ch7_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH8", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch8_mix, ARRAY_SIZE(memif_ul_cm1_ch8_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH9", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch9_mix, ARRAY_SIZE(memif_ul_cm1_ch9_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH10", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch10_mix, ARRAY_SIZE(memif_ul_cm1_ch10_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH11", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch11_mix, ARRAY_SIZE(memif_ul_cm1_ch11_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH12", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch12_mix, ARRAY_SIZE(memif_ul_cm1_ch12_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH13", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch13_mix, ARRAY_SIZE(memif_ul_cm1_ch13_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH14", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch14_mix, ARRAY_SIZE(memif_ul_cm1_ch14_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH15", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch15_mix, ARRAY_SIZE(memif_ul_cm1_ch15_mix)),
	SND_SOC_DAPM_MIXER("UL_CM1_CH16", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm1_ch16_mix, ARRAY_SIZE(memif_ul_cm1_ch16_mix)),
	SND_SOC_DAPM_MUX("CM1_UL_MUX", SND_SOC_NOPM, 0, 0,
			   &ul_cm1_mux_control),

	SND_SOC_DAPM_MIXER("UL_CM2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch1_mix, ARRAY_SIZE(memif_ul_cm2_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch2_mix, ARRAY_SIZE(memif_ul_cm2_ch2_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH3", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch3_mix, ARRAY_SIZE(memif_ul_cm2_ch3_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH4", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch4_mix, ARRAY_SIZE(memif_ul_cm2_ch4_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH5", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch5_mix, ARRAY_SIZE(memif_ul_cm2_ch5_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH6", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch6_mix, ARRAY_SIZE(memif_ul_cm2_ch6_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH7", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch7_mix, ARRAY_SIZE(memif_ul_cm2_ch7_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH8", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch8_mix, ARRAY_SIZE(memif_ul_cm2_ch8_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH9", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch9_mix, ARRAY_SIZE(memif_ul_cm2_ch9_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH10", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch10_mix, ARRAY_SIZE(memif_ul_cm2_ch10_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH11", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch11_mix, ARRAY_SIZE(memif_ul_cm2_ch11_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH12", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch12_mix, ARRAY_SIZE(memif_ul_cm2_ch12_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH13", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch13_mix, ARRAY_SIZE(memif_ul_cm2_ch13_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH14", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch14_mix, ARRAY_SIZE(memif_ul_cm2_ch14_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH15", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch15_mix, ARRAY_SIZE(memif_ul_cm2_ch15_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH16", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch16_mix, ARRAY_SIZE(memif_ul_cm2_ch16_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH17", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch17_mix, ARRAY_SIZE(memif_ul_cm2_ch17_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH18", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch18_mix, ARRAY_SIZE(memif_ul_cm2_ch18_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH19", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch19_mix, ARRAY_SIZE(memif_ul_cm2_ch19_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH20", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch20_mix, ARRAY_SIZE(memif_ul_cm2_ch20_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH21", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch21_mix, ARRAY_SIZE(memif_ul_cm2_ch21_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH22", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch22_mix, ARRAY_SIZE(memif_ul_cm2_ch22_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH23", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch23_mix, ARRAY_SIZE(memif_ul_cm2_ch23_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH24", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch24_mix, ARRAY_SIZE(memif_ul_cm2_ch24_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH25", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch25_mix, ARRAY_SIZE(memif_ul_cm2_ch25_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH26", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch26_mix, ARRAY_SIZE(memif_ul_cm2_ch26_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH27", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch27_mix, ARRAY_SIZE(memif_ul_cm2_ch27_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH28", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch28_mix, ARRAY_SIZE(memif_ul_cm2_ch28_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH29", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch29_mix, ARRAY_SIZE(memif_ul_cm2_ch29_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH30", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch30_mix, ARRAY_SIZE(memif_ul_cm2_ch30_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH31", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch31_mix, ARRAY_SIZE(memif_ul_cm2_ch31_mix)),
	SND_SOC_DAPM_MIXER("UL_CM2_CH32", SND_SOC_NOPM, 0, 0,
			   memif_ul_cm2_ch32_mix, ARRAY_SIZE(memif_ul_cm2_ch32_mix)),
	SND_SOC_DAPM_MUX("CM2_UL_MUX", SND_SOC_NOPM, 0, 0,
			  &ul_cm2_mux_control),

	SND_SOC_DAPM_SUPPLY("CM0_Enable",
			AFE_CM0_CON0, AFE_CM0_ON_SFT, 0,
			ul_cm0_event,
			SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_SUPPLY("CM1_Enable",
			AFE_CM1_CON0, AFE_CM1_ON_SFT, 0,
			ul_cm1_event,
			SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_SUPPLY("CM2_Enable",
			AFE_CM2_CON0, AFE_CM2_ON_SFT, 0,
			ul_cm2_event,
			SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_MIXER("SOF_DMA_UL0", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SOF_DMA_UL1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SOF_DMA_UL2", SND_SOC_NOPM, 0, 0, NULL, 0),
};

static const struct snd_soc_dapm_route mt8196_memif_routes[] = {
	{"UL0", NULL, "UL0_CH1"},
	{"UL0", NULL, "UL0_CH2"},
	/* Normal record */
	{"UL0_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL0_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL0_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL0_CH1", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL0_CH1", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL0_CH1", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL0_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL0_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL0_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL0_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL0_CH2", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL0_CH2", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	/* FM */
	{"UL0_CH1", "CONNSYS_I2S_CH1", "Connsys I2S"},
	{"UL0_CH2", "CONNSYS_I2S_CH2", "Connsys I2S"},

	{"UL0_CH1", "I2SIN0_CH1", "I2SIN0"},
	{"UL0_CH2", "I2SIN0_CH2", "I2SIN0"},
	{"UL0_CH1", "I2SIN1_CH1", "I2SIN1"},
	{"UL0_CH2", "I2SIN1_CH2", "I2SIN1"},

	/* SOF Uplink */
	{"SOF_DMA_UL0", NULL, "UL0_CH1"},
	{"SOF_DMA_UL0", NULL, "UL0_CH2"},

	{"UL1", NULL, "UL1_CH1"},
	{"UL1", NULL, "UL1_CH2"},

	{"UL1_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL1_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},

	{"UL1_CH1", "I2SIN0_CH1", "I2SIN0"},
	{"UL1_CH2", "I2SIN0_CH2", "I2SIN0"},
	{"UL1_CH1", "I2SIN1_CH1", "I2SIN1"},
	{"UL1_CH2", "I2SIN1_CH2", "I2SIN1"},
	{"UL1_CH1", "I2SIN3_CH1", "I2SIN3"},
	{"UL1_CH2", "I2SIN3_CH2", "I2SIN3"},
	{"UL1_CH1", "I2SIN4_CH1", "I2SIN4"},
	{"UL1_CH2", "I2SIN4_CH2", "I2SIN4"},
	{"UL1_CH1", "I2SIN6_CH1", "I2SIN6"},
	{"UL1_CH2", "I2SIN6_CH2", "I2SIN6"},
	{"UL1_CH1", "CONNSYS_I2S_CH1", "Connsys I2S"},
	{"UL1_CH2", "CONNSYS_I2S_CH2", "Connsys I2S"},

	/* SOF Uplink */
	{"SOF_DMA_UL1", NULL, "UL1_CH1"},
	{"SOF_DMA_UL1", NULL, "UL1_CH2"},

	{"UL2", NULL, "UL2_CH1"},
	{"UL2", NULL, "UL2_CH2"},
	{"UL2_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL2_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL2_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL2_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL2_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL2_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL2_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},


	/* SOF Uplink */
	{"SOF_DMA_UL2", NULL, "UL2_CH1"},
	{"SOF_DMA_UL2", NULL, "UL2_CH2"},

	{"UL3", NULL, "UL3_CH1"},
	{"UL3", NULL, "UL3_CH2"},

	{"UL3_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL3_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL3_CH1", "I2SIN0_CH1", "I2SIN0"},
	{"UL3_CH2", "I2SIN0_CH2", "I2SIN0"},
	{"UL3_CH1", "I2SIN1_CH1", "I2SIN1"},
	{"UL3_CH2", "I2SIN1_CH2", "I2SIN1"},
	{"UL3_CH1", "I2SIN3_CH1", "I2SIN3"},
	{"UL3_CH2", "I2SIN3_CH2", "I2SIN3"},
	{"UL3_CH1", "I2SIN4_CH1", "I2SIN4"},
	{"UL3_CH2", "I2SIN4_CH2", "I2SIN4"},

	{"UL4", NULL, "UL4_CH1"},
	{"UL4", NULL, "UL4_CH2"},
	{"UL4_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL4_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL4_CH1", "I2SIN0_CH1", "I2SIN0"},
	{"UL4_CH2", "I2SIN0_CH2", "I2SIN0"},

	{"UL5", NULL, "UL5_CH1"},
	{"UL5", NULL, "UL5_CH2"},

	{"UL5_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL5_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL5_CH1", "I2SIN3_CH1", "I2SIN3"},
	{"UL5_CH2", "I2SIN3_CH2", "I2SIN3"},

	{"UL6", NULL, "UL6_CH1"},
	{"UL6", NULL, "UL6_CH2"},
	{"UL6_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL6_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL6_CH1", "CONNSYS_I2S_CH1", "Connsys I2S"},
	{"UL6_CH2", "CONNSYS_I2S_CH2", "Connsys I2S"},

	{"UL7", NULL, "UL7_CH1"},
	{"UL7", NULL, "UL7_CH2"},
	{"UL7_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL7_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL7_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL7_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL7_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL7_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL7_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL7_CH1", "CONNSYS_I2S_CH1", "Connsys I2S"},
	{"UL7_CH2", "CONNSYS_I2S_CH2", "Connsys I2S"},

	{"UL8", NULL, "CM0_UL_MUX"},
	{"CM0_UL_MUX", "CM0_2CH_PATH", "UL8_CH1"},
	{"CM0_UL_MUX", "CM0_2CH_PATH", "UL8_CH2"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH1"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH2"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH3"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH4"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH5"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH6"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH7"},
	{"CM0_UL_MUX", "CM0_8CH_PATH", "UL_CM0_CH8"},
	{"UL_CM0_CH1", NULL, "CM0_Enable"},
	{"UL_CM0_CH2", NULL, "CM0_Enable"},
	{"UL_CM0_CH3", NULL, "CM0_Enable"},
	{"UL_CM0_CH4", NULL, "CM0_Enable"},
	{"UL_CM0_CH5", NULL, "CM0_Enable"},
	{"UL_CM0_CH6", NULL, "CM0_Enable"},
	{"UL_CM0_CH7", NULL, "CM0_Enable"},
	{"UL_CM0_CH8", NULL, "CM0_Enable"},

	/* UL9 */
	{"UL9", NULL, "CM1_UL_MUX"},
	{"CM1_UL_MUX", "CM1_2CH_PATH", "UL9_CH1"},
	{"CM1_UL_MUX", "CM1_2CH_PATH", "UL9_CH2"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH1"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH2"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH3"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH4"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH5"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH6"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH7"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH8"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH9"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH10"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH11"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH12"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH13"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH14"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH15"},
	{"CM1_UL_MUX", "CM1_16CH_PATH", "UL_CM1_CH16"},

	{"UL_CM1_CH1", NULL, "CM1_Enable"},
	{"UL_CM1_CH2", NULL, "CM1_Enable"},
	{"UL_CM1_CH3", NULL, "CM1_Enable"},
	{"UL_CM1_CH4", NULL, "CM1_Enable"},
	{"UL_CM1_CH5", NULL, "CM1_Enable"},
	{"UL_CM1_CH6", NULL, "CM1_Enable"},
	{"UL_CM1_CH7", NULL, "CM1_Enable"},
	{"UL_CM1_CH8", NULL, "CM1_Enable"},
	{"UL_CM1_CH9", NULL, "CM1_Enable"},
	{"UL_CM1_CH10", NULL, "CM1_Enable"},
	{"UL_CM1_CH11", NULL, "CM1_Enable"},
	{"UL_CM1_CH12", NULL, "CM1_Enable"},
	{"UL_CM1_CH13", NULL, "CM1_Enable"},
	{"UL_CM1_CH14", NULL, "CM1_Enable"},
	{"UL_CM1_CH15", NULL, "CM1_Enable"},
	{"UL_CM1_CH16", NULL, "CM1_Enable"},

	/* UL9 o36o37 <- ADDA */
	{"UL9_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL9_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL9_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL9_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL9_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL9_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL9_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},

	{"UL10", NULL, "CM2_UL_MUX"},
	{"CM2_UL_MUX", "CM2_2CH_PATH", "UL10_CH1"},
	{"CM2_UL_MUX", "CM2_2CH_PATH", "UL10_CH2"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH1"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH2"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH3"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH4"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH5"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH6"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH7"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH8"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH9"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH10"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH11"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH12"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH13"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH14"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH15"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH16"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH17"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH18"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH19"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH20"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH21"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH22"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH23"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH24"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH25"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH26"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH27"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH28"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH29"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH30"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH31"},
	{"CM2_UL_MUX", "CM2_32CH_PATH", "UL_CM2_CH32"},
	{"UL_CM2_CH1", NULL, "CM2_Enable"},
	{"UL_CM2_CH2", NULL, "CM2_Enable"},
	{"UL_CM2_CH3", NULL, "CM2_Enable"},
	{"UL_CM2_CH4", NULL, "CM2_Enable"},
	{"UL_CM2_CH5", NULL, "CM2_Enable"},
	{"UL_CM2_CH6", NULL, "CM2_Enable"},
	{"UL_CM2_CH7", NULL, "CM2_Enable"},
	{"UL_CM2_CH8", NULL, "CM2_Enable"},
	{"UL_CM2_CH9", NULL, "CM2_Enable"},
	{"UL_CM2_CH10", NULL, "CM2_Enable"},
	{"UL_CM2_CH11", NULL, "CM2_Enable"},
	{"UL_CM2_CH12", NULL, "CM2_Enable"},
	{"UL_CM2_CH13", NULL, "CM2_Enable"},
	{"UL_CM2_CH14", NULL, "CM2_Enable"},
	{"UL_CM2_CH15", NULL, "CM2_Enable"},
	{"UL_CM2_CH16", NULL, "CM2_Enable"},
	{"UL_CM2_CH17", NULL, "CM2_Enable"},
	{"UL_CM2_CH18", NULL, "CM2_Enable"},
	{"UL_CM2_CH19", NULL, "CM2_Enable"},
	{"UL_CM2_CH20", NULL, "CM2_Enable"},
	{"UL_CM2_CH21", NULL, "CM2_Enable"},
	{"UL_CM2_CH22", NULL, "CM2_Enable"},
	{"UL_CM2_CH23", NULL, "CM2_Enable"},
	{"UL_CM2_CH24", NULL, "CM2_Enable"},
	{"UL_CM2_CH25", NULL, "CM2_Enable"},
	{"UL_CM2_CH26", NULL, "CM2_Enable"},
	{"UL_CM2_CH27", NULL, "CM2_Enable"},
	{"UL_CM2_CH28", NULL, "CM2_Enable"},
	{"UL_CM2_CH29", NULL, "CM2_Enable"},
	{"UL_CM2_CH30", NULL, "CM2_Enable"},
	{"UL_CM2_CH31", NULL, "CM2_Enable"},
	{"UL_CM2_CH32", NULL, "CM2_Enable"},

	/* UL10 o38o39 <- ADDA */
	{"UL10_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL10_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL10_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL10_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL10_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL10_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL10_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},

	{"UL24", NULL, "UL24_CH1"},
	{"UL24", NULL, "UL24_CH2"},
	{"UL24_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL24_CH1", "I2SIN6_CH1", "I2SIN6"},
	{"UL24_CH2", "I2SIN6_CH2", "I2SIN6"},
	{"UL24_CH1", "I2SIN0_CH1", "I2SIN0"},
	{"UL24_CH2", "I2SIN0_CH2", "I2SIN0"},

	{"UL25", NULL, "UL25_CH1"},
	{"UL25", NULL, "UL25_CH2"},
	{"UL25_CH1", "I2SIN6_CH1", "I2SIN6"},
	{"UL25_CH2", "I2SIN6_CH2", "I2SIN6"},
	{"UL25_CH1", "I2SIN0_CH1", "I2SIN0"},
	{"UL25_CH2", "I2SIN0_CH2", "I2SIN0"},

	{"UL26", NULL, "UL26_CH1"},
	{"UL26", NULL, "UL26_CH2"},
	{"UL26_CH1", "I2SIN6_CH1", "I2SIN6"},
	{"UL26_CH2", "I2SIN6_CH2", "I2SIN6"},
	{"UL26_CH1", "I2SIN0_CH1", "I2SIN0"},
	{"UL26_CH2", "I2SIN0_CH2", "I2SIN0"},

	{"DSP_DL", "DSP_DL0", "DL0"},
	{"DSP_DL", "DSP_DL2", "DL2"},
	{"DSP_DL", "DSP_DL1", "DL1"},
	{"DSP_DL", "DSP_DL6", "DL6"},
	{"DSP_DL", "DSP_DL3", "DL3"},
	{"DSP_DL", "DSP_DL_24CH", "DL_24CH"},

	{"UL_CM0", NULL, "UL_CM0_CH1"},
	{"UL_CM0", NULL, "UL_CM0_CH2"},
	{"UL_CM0", NULL, "UL_CM0_CH3"},
	{"UL_CM0", NULL, "UL_CM0_CH4"},
	{"UL_CM0", NULL, "UL_CM0_CH5"},
	{"UL_CM0", NULL, "UL_CM0_CH6"},
	{"UL_CM0", NULL, "UL_CM0_CH7"},
	{"UL_CM0", NULL, "UL_CM0_CH8"},
	{"UL_CM0_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM0_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM0_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM0_CH1", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM0_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM0_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM0_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM0_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM0_CH3", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM0_CH3", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM0_CH3", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM0_CH3", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM0_CH4", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM0_CH4", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM0_CH4", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM0_CH4", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},

	{"UL_CM1", NULL, "UL_CM1_CH1"},
	{"UL_CM1", NULL, "UL_CM1_CH2"},
	{"UL_CM1", NULL, "UL_CM1_CH3"},
	{"UL_CM1", NULL, "UL_CM1_CH4"},
	{"UL_CM1", NULL, "UL_CM1_CH5"},
	{"UL_CM1", NULL, "UL_CM1_CH6"},
	{"UL_CM1", NULL, "UL_CM1_CH7"},
	{"UL_CM1", NULL, "UL_CM1_CH8"},
	{"UL_CM1", NULL, "UL_CM1_CH9"},
	{"UL_CM1", NULL, "UL_CM1_CH10"},
	{"UL_CM1", NULL, "UL_CM1_CH11"},
	{"UL_CM1", NULL, "UL_CM1_CH12"},
	{"UL_CM1", NULL, "UL_CM1_CH13"},
	{"UL_CM1", NULL, "UL_CM1_CH14"},
	{"UL_CM1", NULL, "UL_CM1_CH15"},
	{"UL_CM1", NULL, "UL_CM1_CH16"},
	{"UL_CM1_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM1_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM1_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH1", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH1", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH1", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM1_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM1_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH2", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH2", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH3", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM1_CH3", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM1_CH3", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH3", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH3", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH3", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH4", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM1_CH4", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM1_CH4", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH4", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH4", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH4", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH5", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM1_CH5", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM1_CH5", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH5", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH5", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH5", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH6", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM1_CH6", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM1_CH6", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH6", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM1_CH6", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM1_CH6", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},


	{"UL_CM2", NULL, "UL_CM2_CH1"},
	{"UL_CM2", NULL, "UL_CM2_CH2"},
	{"UL_CM2", NULL, "UL_CM2_CH3"},
	{"UL_CM2", NULL, "UL_CM2_CH4"},
	{"UL_CM2", NULL, "UL_CM2_CH5"},
	{"UL_CM2", NULL, "UL_CM2_CH6"},
	{"UL_CM2", NULL, "UL_CM2_CH7"},
	{"UL_CM2", NULL, "UL_CM2_CH8"},
	{"UL_CM2", NULL, "UL_CM2_CH9"},
	{"UL_CM2", NULL, "UL_CM2_CH10"},
	{"UL_CM2", NULL, "UL_CM2_CH11"},
	{"UL_CM2", NULL, "UL_CM2_CH12"},
	{"UL_CM2", NULL, "UL_CM2_CH13"},
	{"UL_CM2", NULL, "UL_CM2_CH14"},
	{"UL_CM2", NULL, "UL_CM2_CH15"},
	{"UL_CM2", NULL, "UL_CM2_CH16"},
	{"UL_CM2", NULL, "UL_CM2_CH17"},
	{"UL_CM2", NULL, "UL_CM2_CH18"},
	{"UL_CM2", NULL, "UL_CM2_CH19"},
	{"UL_CM2", NULL, "UL_CM2_CH20"},
	{"UL_CM2", NULL, "UL_CM2_CH21"},
	{"UL_CM2", NULL, "UL_CM2_CH22"},
	{"UL_CM2", NULL, "UL_CM2_CH23"},
	{"UL_CM2", NULL, "UL_CM2_CH24"},
	{"UL_CM2", NULL, "UL_CM2_CH25"},
	{"UL_CM2", NULL, "UL_CM2_CH26"},
	{"UL_CM2", NULL, "UL_CM2_CH27"},
	{"UL_CM2", NULL, "UL_CM2_CH28"},
	{"UL_CM2", NULL, "UL_CM2_CH29"},
	{"UL_CM2", NULL, "UL_CM2_CH30"},
	{"UL_CM2", NULL, "UL_CM2_CH31"},
	{"UL_CM2", NULL, "UL_CM2_CH32"},
	{"UL_CM2_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM2_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM2_CH1", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH1", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH1", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH1", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM2_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM2_CH2", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH2", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH2", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH2", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH3", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM2_CH3", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM2_CH3", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH3", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH3", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH3", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH4", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM2_CH4", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM2_CH4", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH4", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH4", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH4", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH5", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM2_CH5", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM2_CH5", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH5", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH5", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH5", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH6", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"UL_CM2_CH6", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"UL_CM2_CH6", "ADDA_UL_CH3", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH6", "ADDA_UL_CH4", "ADDA_CH34_UL_Mux"},
	{"UL_CM2_CH6", "ADDA_UL_CH5", "ADDA_CH56_UL_Mux"},
	{"UL_CM2_CH6", "ADDA_UL_CH6", "ADDA_CH56_UL_Mux"},
};

static const struct mtk_base_memif_data memif_data[MT8196_MEMIF_NUM] = {
	[MT8196_MEMIF_DL0] = {
		.name = "DL0",
		.id = MT8196_MEMIF_DL0,
		.reg_ofs_base = AFE_DL0_BASE,
		.reg_ofs_cur = AFE_DL0_CUR,
		.reg_ofs_end = AFE_DL0_END,
		.reg_ofs_base_msb = AFE_DL0_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL0_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL0_END_MSB,
		.fs_reg = AFE_DL0_CON0,
		.fs_shift = DL0_SEL_FS_SFT,
		.fs_maskbit = DL0_SEL_FS_MASK,
		.mono_reg = AFE_DL0_CON0,
		.mono_shift = DL0_MONO_SFT,
		.enable_reg = AFE_DL0_CON0,
		.enable_shift = DL0_ON_SFT,
		.hd_reg = AFE_DL0_CON0,
		.hd_mask = DL0_HD_MODE_MASK,
		.hd_shift = DL0_HD_MODE_SFT,
		.hd_align_reg = AFE_DL0_CON0,
		.hd_align_mshift = DL0_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL0_CON0,
		.pbuf_mask = DL0_PBUF_SIZE_MASK,
		.pbuf_shift = DL0_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL0_CON0,
		.minlen_mask = DL0_MINLEN_MASK,
		.minlen_shift = DL0_MINLEN_SFT,
		.maxlen_reg = AFE_DL0_CON0,
		.maxlen_mask = DL0_MAXLEN_MASK,
		.maxlen_shift = DL0_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL1] = {
		.name = "DL1",
		.id = MT8196_MEMIF_DL1,
		.reg_ofs_base = AFE_DL1_BASE,
		.reg_ofs_cur = AFE_DL1_CUR,
		.reg_ofs_end = AFE_DL1_END,
		.reg_ofs_base_msb = AFE_DL1_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL1_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL1_END_MSB,
		.fs_reg = AFE_DL1_CON0,
		.fs_shift = DL1_SEL_FS_SFT,
		.fs_maskbit = DL1_SEL_FS_MASK,
		.mono_reg = AFE_DL1_CON0,
		.mono_shift = DL1_MONO_SFT,
		.enable_reg = AFE_DL1_CON0,
		.enable_shift = DL1_ON_SFT,
		.hd_reg = AFE_DL1_CON0,
		.hd_mask = DL1_HD_MODE_MASK,
		.hd_shift = DL1_HD_MODE_SFT,
		.hd_align_reg = AFE_DL1_CON0,
		.hd_align_mshift = DL1_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL1_CON0,
		.pbuf_mask = DL1_PBUF_SIZE_MASK,
		.pbuf_shift = DL1_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL1_CON0,
		.minlen_mask = DL1_MINLEN_MASK,
		.minlen_shift = DL1_MINLEN_SFT,
		.maxlen_reg = AFE_DL1_CON0,
		.maxlen_mask = DL1_MAXLEN_MASK,
		.maxlen_shift = DL1_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL2] = {
		.name = "DL2",
		.id = MT8196_MEMIF_DL2,
		.reg_ofs_base = AFE_DL2_BASE,
		.reg_ofs_cur = AFE_DL2_CUR,
		.reg_ofs_end = AFE_DL2_END,
		.reg_ofs_base_msb = AFE_DL2_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL2_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL2_END_MSB,
		.fs_reg = AFE_DL2_CON0,
		.fs_shift = DL2_SEL_FS_SFT,
		.fs_maskbit = DL2_SEL_FS_MASK,
		.mono_reg = AFE_DL2_CON0,
		.mono_shift = DL2_MONO_SFT,
		.enable_reg = AFE_DL2_CON0,
		.enable_shift = DL2_ON_SFT,
		.hd_reg = AFE_DL2_CON0,
		.hd_mask = DL2_HD_MODE_MASK,
		.hd_shift = DL2_HD_MODE_SFT,
		.hd_align_reg = AFE_DL2_CON0,
		.hd_align_mshift = DL2_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL2_CON0,
		.pbuf_mask = DL2_PBUF_SIZE_MASK,
		.pbuf_shift = DL2_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL2_CON0,
		.minlen_mask = DL2_MINLEN_MASK,
		.minlen_shift = DL2_MINLEN_SFT,
		.maxlen_reg = AFE_DL2_CON0,
		.maxlen_mask = DL2_MAXLEN_MASK,
		.maxlen_shift = DL2_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL3] = {
		.name = "DL3",
		.id = MT8196_MEMIF_DL3,
		.reg_ofs_base = AFE_DL3_BASE,
		.reg_ofs_cur = AFE_DL3_CUR,
		.reg_ofs_end = AFE_DL3_END,
		.reg_ofs_base_msb = AFE_DL3_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL3_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL3_END_MSB,
		.fs_reg = AFE_DL3_CON0,
		.fs_shift = DL3_SEL_FS_SFT,
		.fs_maskbit = DL3_SEL_FS_MASK,
		.mono_reg = AFE_DL3_CON0,
		.mono_shift = DL3_MONO_SFT,
		.enable_reg = AFE_DL3_CON0,
		.enable_shift = DL3_ON_SFT,
		.hd_reg = AFE_DL3_CON0,
		.hd_mask = DL3_HD_MODE_MASK,
		.hd_shift = DL3_HD_MODE_SFT,
		.hd_align_reg = AFE_DL3_CON0,
		.hd_align_mshift = DL3_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL3_CON0,
		.pbuf_mask = DL3_PBUF_SIZE_MASK,
		.pbuf_shift = DL3_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL3_CON0,
		.minlen_mask = DL3_MINLEN_MASK,
		.minlen_shift = DL3_MINLEN_SFT,
		.maxlen_reg = AFE_DL3_CON0,
		.maxlen_mask = DL3_MAXLEN_MASK,
		.maxlen_shift = DL3_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL4] = {
		.name = "DL4",
		.id = MT8196_MEMIF_DL4,
		.reg_ofs_base = AFE_DL4_BASE,
		.reg_ofs_cur = AFE_DL4_CUR,
		.reg_ofs_end = AFE_DL4_END,
		.reg_ofs_base_msb = AFE_DL4_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL4_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL4_END_MSB,
		.fs_reg = AFE_DL4_CON0,
		.fs_shift = DL4_SEL_FS_SFT,
		.fs_maskbit = DL4_SEL_FS_MASK,
		.mono_reg = AFE_DL4_CON0,
		.mono_shift = DL4_MONO_SFT,
		.enable_reg = AFE_DL4_CON0,
		.enable_shift = DL4_ON_SFT,
		.hd_reg = AFE_DL4_CON0,
		.hd_mask = DL4_HD_MODE_MASK,
		.hd_shift = DL4_HD_MODE_SFT,
		.hd_align_reg = AFE_DL4_CON0,
		.hd_align_mshift = DL4_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL4_CON0,
		.pbuf_mask = DL4_PBUF_SIZE_MASK,
		.pbuf_shift = DL4_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL4_CON0,
		.minlen_mask = DL4_MINLEN_MASK,
		.minlen_shift = DL4_MINLEN_SFT,
		.maxlen_reg = AFE_DL4_CON0,
		.maxlen_mask = DL4_MAXLEN_MASK,
		.maxlen_shift = DL4_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL5] = {
		.name = "DL5",
		.id = MT8196_MEMIF_DL5,
		.reg_ofs_base = AFE_DL5_BASE,
		.reg_ofs_cur = AFE_DL5_CUR,
		.reg_ofs_end = AFE_DL5_END,
		.reg_ofs_base_msb = AFE_DL5_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL5_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL5_END_MSB,
		.fs_reg = AFE_DL5_CON0,
		.fs_shift = DL5_SEL_FS_SFT,
		.fs_maskbit = DL5_SEL_FS_MASK,
		.mono_reg = AFE_DL5_CON0,
		.mono_shift = DL5_MONO_SFT,
		.enable_reg = AFE_DL5_CON0,
		.enable_shift = DL5_ON_SFT,
		.hd_reg = AFE_DL5_CON0,
		.hd_mask = DL5_HD_MODE_MASK,
		.hd_shift = DL5_HD_MODE_SFT,
		.hd_align_reg = AFE_DL5_CON0,
		.hd_align_mshift = DL5_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL5_CON0,
		.pbuf_mask = DL5_PBUF_SIZE_MASK,
		.pbuf_shift = DL5_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL5_CON0,
		.minlen_mask = DL5_MINLEN_MASK,
		.minlen_shift = DL5_MINLEN_SFT,
		.maxlen_reg = AFE_DL5_CON0,
		.maxlen_mask = DL5_MAXLEN_MASK,
		.maxlen_shift = DL5_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL6] = {
		.name = "DL6",
		.id = MT8196_MEMIF_DL6,
		.reg_ofs_base = AFE_DL6_BASE,
		.reg_ofs_cur = AFE_DL6_CUR,
		.reg_ofs_end = AFE_DL6_END,
		.reg_ofs_base_msb = AFE_DL6_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL6_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL6_END_MSB,
		.fs_reg = AFE_DL6_CON0,
		.fs_shift = DL6_SEL_FS_SFT,
		.fs_maskbit = DL6_SEL_FS_MASK,
		.mono_reg = AFE_DL6_CON0,
		.mono_shift = DL6_MONO_SFT,
		.enable_reg = AFE_DL6_CON0,
		.enable_shift = DL6_ON_SFT,
		.hd_reg = AFE_DL6_CON0,
		.hd_mask = DL6_HD_MODE_MASK,
		.hd_shift = DL6_HD_MODE_SFT,
		.hd_align_reg = AFE_DL6_CON0,
		.hd_align_mshift = DL6_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL6_CON0,
		.pbuf_mask = DL6_PBUF_SIZE_MASK,
		.pbuf_shift = DL6_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL6_CON0,
		.minlen_mask = DL6_MINLEN_MASK,
		.minlen_shift = DL6_MINLEN_SFT,
		.maxlen_reg = AFE_DL6_CON0,
		.maxlen_mask = DL6_MAXLEN_MASK,
		.maxlen_shift = DL6_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL7] = {
		.name = "DL7",
		.id = MT8196_MEMIF_DL7,
		.reg_ofs_base = AFE_DL7_BASE,
		.reg_ofs_cur = AFE_DL7_CUR,
		.reg_ofs_end = AFE_DL7_END,
		.reg_ofs_base_msb = AFE_DL7_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL7_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL7_END_MSB,
		.fs_reg = AFE_DL7_CON0,
		.fs_shift = DL7_SEL_FS_SFT,
		.fs_maskbit = DL7_SEL_FS_MASK,
		.mono_reg = AFE_DL7_CON0,
		.mono_shift = DL7_MONO_SFT,
		.enable_reg = AFE_DL7_CON0,
		.enable_shift = DL7_ON_SFT,
		.hd_reg = AFE_DL7_CON0,
		.hd_mask = DL7_HD_MODE_MASK,
		.hd_shift = DL7_HD_MODE_SFT,
		.hd_align_reg = AFE_DL7_CON0,
		.hd_align_mshift = DL7_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL7_CON0,
		.pbuf_mask = DL7_PBUF_SIZE_MASK,
		.pbuf_shift = DL7_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL7_CON0,
		.minlen_mask = DL7_MINLEN_MASK,
		.minlen_shift = DL7_MINLEN_SFT,
		.maxlen_reg = AFE_DL7_CON0,
		.maxlen_mask = DL7_MAXLEN_MASK,
		.maxlen_shift = DL7_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL8] = {
		.name = "DL8",
		.id = MT8196_MEMIF_DL8,
		.reg_ofs_base = AFE_DL8_BASE,
		.reg_ofs_cur = AFE_DL8_CUR,
		.reg_ofs_end = AFE_DL8_END,
		.reg_ofs_base_msb = AFE_DL8_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL8_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL8_END_MSB,
		.fs_reg = AFE_DL8_CON0,
		.fs_shift = DL8_SEL_FS_SFT,
		.fs_maskbit = DL8_SEL_FS_MASK,
		.mono_reg = AFE_DL8_CON0,
		.mono_shift = DL8_MONO_SFT,
		.enable_reg = AFE_DL8_CON0,
		.enable_shift = DL8_ON_SFT,
		.hd_reg = AFE_DL8_CON0,
		.hd_mask = DL8_HD_MODE_MASK,
		.hd_shift = DL8_HD_MODE_SFT,
		.hd_align_reg = AFE_DL8_CON0,
		.hd_align_mshift = DL8_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL8_CON0,
		.pbuf_mask = DL8_PBUF_SIZE_MASK,
		.pbuf_shift = DL8_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL8_CON0,
		.minlen_mask = DL8_MINLEN_MASK,
		.minlen_shift = DL8_MINLEN_SFT,
		.maxlen_reg = AFE_DL8_CON0,
		.maxlen_mask = DL8_MAXLEN_MASK,
		.maxlen_shift = DL8_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL23] = {
		.name = "DL23",
		.id = MT8196_MEMIF_DL23,
		.reg_ofs_base = AFE_DL23_BASE,
		.reg_ofs_cur = AFE_DL23_CUR,
		.reg_ofs_end = AFE_DL23_END,
		.reg_ofs_base_msb = AFE_DL23_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL23_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL23_END_MSB,
		.fs_reg = AFE_DL23_CON0,
		.fs_shift = DL23_SEL_FS_SFT,
		.fs_maskbit = DL23_SEL_FS_MASK,
		.mono_reg = AFE_DL23_CON0,
		.mono_shift = DL23_MONO_SFT,
		.enable_reg = AFE_DL23_CON0,
		.enable_shift = DL23_ON_SFT,
		.hd_reg = AFE_DL23_CON0,
		.hd_mask = DL23_HD_MODE_MASK,
		.hd_shift = DL23_HD_MODE_SFT,
		.hd_align_reg = AFE_DL23_CON0,
		.hd_align_mshift = DL23_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL23_CON0,
		.pbuf_mask = DL23_PBUF_SIZE_MASK,
		.pbuf_shift = DL23_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL23_CON0,
		.minlen_mask = DL23_MINLEN_MASK,
		.minlen_shift = DL23_MINLEN_SFT,
		.maxlen_reg = AFE_DL23_CON0,
		.maxlen_mask = DL23_MAXLEN_MASK,
		.maxlen_shift = DL23_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL24] = {
		.name = "DL24",
		.id = MT8196_MEMIF_DL24,
		.reg_ofs_base = AFE_DL24_BASE,
		.reg_ofs_cur = AFE_DL24_CUR,
		.reg_ofs_end = AFE_DL24_END,
		.reg_ofs_base_msb = AFE_DL24_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL24_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL24_END_MSB,
		.fs_reg = AFE_DL24_CON0,
		.fs_shift = DL24_SEL_FS_SFT,
		.fs_maskbit = DL24_SEL_FS_MASK,
		.mono_reg = AFE_DL24_CON0,
		.mono_shift = DL24_MONO_SFT,
		.enable_reg = AFE_DL24_CON0,
		.enable_shift = DL24_ON_SFT,
		.hd_reg = AFE_DL24_CON0,
		.hd_mask = DL24_HD_MODE_MASK,
		.hd_shift = DL24_HD_MODE_SFT,
		.hd_align_reg = AFE_DL24_CON0,
		.hd_align_mshift = DL24_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL24_CON0,
		.pbuf_mask = DL24_PBUF_SIZE_MASK,
		.pbuf_shift = DL24_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL24_CON0,
		.minlen_mask = DL24_MINLEN_MASK,
		.minlen_shift = DL24_MINLEN_SFT,
		.maxlen_reg = AFE_DL24_CON0,
		.maxlen_mask = DL24_MAXLEN_MASK,
		.maxlen_shift = DL24_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL25] = {
		.name = "DL25",
		.id = MT8196_MEMIF_DL25,
		.reg_ofs_base = AFE_DL25_BASE,
		.reg_ofs_cur = AFE_DL25_CUR,
		.reg_ofs_end = AFE_DL25_END,
		.reg_ofs_base_msb = AFE_DL25_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL25_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL25_END_MSB,
		.fs_reg = AFE_DL25_CON0,
		.fs_shift = DL25_SEL_FS_SFT,
		.fs_maskbit = DL25_SEL_FS_MASK,
		.mono_reg = AFE_DL25_CON0,
		.mono_shift = DL25_MONO_SFT,
		.enable_reg = AFE_DL25_CON0,
		.enable_shift = DL25_ON_SFT,
		.hd_reg = AFE_DL25_CON0,
		.hd_mask = DL25_HD_MODE_MASK,
		.hd_shift = DL25_HD_MODE_SFT,
		.hd_align_reg = AFE_DL25_CON0,
		.hd_align_mshift = DL25_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL25_CON0,
		.pbuf_mask = DL25_PBUF_SIZE_MASK,
		.pbuf_shift = DL25_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL25_CON0,
		.minlen_mask = DL25_MINLEN_MASK,
		.minlen_shift = DL25_MINLEN_SFT,
		.maxlen_reg = AFE_DL25_CON0,
		.maxlen_mask = DL25_MAXLEN_MASK,
		.maxlen_shift = DL25_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL26] = {
		.name = "DL26",
		.id = MT8196_MEMIF_DL26,
		.reg_ofs_base = AFE_DL26_BASE,
		.reg_ofs_cur = AFE_DL26_CUR,
		.reg_ofs_end = AFE_DL26_END,
		.reg_ofs_base_msb = AFE_DL26_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL26_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL26_END_MSB,
		.fs_reg = AFE_DL26_CON0,
		.fs_shift = DL26_SEL_FS_SFT,
		.fs_maskbit = DL26_SEL_FS_MASK,
		.mono_reg = AFE_DL26_CON0,
		.mono_shift = DL26_MONO_SFT,
		.enable_reg = AFE_DL26_CON0,
		.enable_shift = DL26_ON_SFT,
		.hd_reg = AFE_DL26_CON0,
		.hd_mask = DL26_HD_MODE_MASK,
		.hd_shift = DL26_HD_MODE_SFT,
		.hd_align_reg = AFE_DL26_CON0,
		.hd_align_mshift = DL26_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL26_CON0,
		.pbuf_mask = DL26_PBUF_SIZE_MASK,
		.pbuf_shift = DL26_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL26_CON0,
		.minlen_mask = DL26_MINLEN_MASK,
		.minlen_shift = DL26_MINLEN_SFT,
		.maxlen_reg = AFE_DL26_CON0,
		.maxlen_mask = DL26_MAXLEN_MASK,
		.maxlen_shift = DL26_MAXLEN_SFT,
	},
	[MT8196_MEMIF_DL_4CH] = {
		.name = "DL_4CH",
		.id = MT8196_MEMIF_DL_4CH,
		.reg_ofs_base = AFE_DL_4CH_BASE,
		.reg_ofs_cur = AFE_DL_4CH_CUR,
		.reg_ofs_end = AFE_DL_4CH_END,
		.reg_ofs_base_msb = AFE_DL_4CH_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL_4CH_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL_4CH_END_MSB,
		.fs_reg = AFE_DL_4CH_CON0,
		.fs_shift = DL_4CH_SEL_FS_SFT,
		.fs_maskbit = DL_4CH_SEL_FS_MASK,
		.mono_reg = -1,
		.mono_shift = -1,
		.enable_reg = AFE_DL_4CH_CON0,
		.enable_shift = DL_4CH_ON_SFT,
		.hd_reg = AFE_DL_4CH_CON0,
		.hd_mask = DL_4CH_HD_MODE_MASK,
		.hd_shift = DL_4CH_HD_MODE_SFT,
		.hd_align_reg = AFE_DL_4CH_CON0,
		.hd_align_mshift = DL_4CH_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL_4CH_CON0,
		.pbuf_mask = DL_4CH_PBUF_SIZE_MASK,
		.pbuf_shift = DL_4CH_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL_4CH_CON0,
		.minlen_mask = DL_4CH_MINLEN_MASK,
		.minlen_shift = DL_4CH_MINLEN_SFT,
		.maxlen_reg = AFE_DL_4CH_CON0,
		.maxlen_mask = DL_4CH_MAXLEN_MASK,
		.maxlen_shift = DL_4CH_MAXLEN_SFT,
		.ch_num_reg = AFE_DL_4CH_CON0,
		.ch_num_maskbit = DL_4CH_NUM_MASK,
		.ch_num_shift = DL_4CH_NUM_SFT,
	},
	[MT8196_MEMIF_DL_24CH] = {
		.name = "DL_24CH",
		.id = MT8196_MEMIF_DL_24CH,
		.reg_ofs_base = AFE_DL_24CH_BASE,
		.reg_ofs_cur = AFE_DL_24CH_CUR,
		.reg_ofs_end = AFE_DL_24CH_END,
		.reg_ofs_base_msb = AFE_DL_24CH_BASE_MSB,
		.reg_ofs_cur_msb = AFE_DL_24CH_CUR_MSB,
		.reg_ofs_end_msb = AFE_DL_24CH_END_MSB,
		.fs_reg = AFE_DL_24CH_CON0,
		.fs_shift = DL_24CH_SEL_FS_SFT,
		.fs_maskbit = DL_24CH_SEL_FS_MASK,
		.mono_reg = -1,
		.mono_shift = -1,
		.enable_reg = AFE_DL_24CH_CON0,
		.enable_shift = DL_24CH_ON_SFT,
		.hd_reg = AFE_DL_24CH_CON0,
		.hd_mask = DL_24CH_HD_MODE_MASK,
		.hd_shift = DL_24CH_HD_MODE_SFT,
		.hd_align_reg = AFE_DL_24CH_CON0,
		.hd_align_mshift = DL_24CH_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_DL_24CH_CON0,
		.pbuf_mask = DL_24CH_PBUF_SIZE_MASK,
		.pbuf_shift = DL_24CH_PBUF_SIZE_SFT,
		.minlen_reg = AFE_DL_24CH_CON0,
		.minlen_mask = DL_24CH_MINLEN_MASK,
		.minlen_shift = DL_24CH_MINLEN_SFT,
		.maxlen_reg = AFE_DL_24CH_CON0,
		.maxlen_mask = DL_24CH_MAXLEN_MASK,
		.maxlen_shift = DL_24CH_MAXLEN_SFT,
		.ch_num_reg = AFE_DL_24CH_CON0,
		.ch_num_maskbit = DL_24CH_NUM_MASK,
		.ch_num_shift = DL_24CH_NUM_SFT,
	},
	[MT8196_MEMIF_VUL0] = {
		.name = "VUL0",
		.id = MT8196_MEMIF_VUL0,
		.reg_ofs_base = AFE_VUL0_BASE,
		.reg_ofs_cur = AFE_VUL0_CUR,
		.reg_ofs_end = AFE_VUL0_END,
		.reg_ofs_base_msb = AFE_VUL0_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL0_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL0_END_MSB,
		.fs_reg = AFE_VUL0_CON0,
		.fs_shift = VUL0_SEL_FS_SFT,
		.fs_maskbit = VUL0_SEL_FS_MASK,
		.mono_reg = AFE_VUL0_CON0,
		.mono_shift = VUL0_MONO_SFT,
		.enable_reg = AFE_VUL0_CON0,
		.enable_shift = VUL0_ON_SFT,
		.hd_reg = AFE_VUL0_CON0,
		.hd_mask = VUL0_HD_MODE_MASK,
		.hd_shift = VUL0_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL0_CON0,
		.hd_align_mshift = VUL0_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL1] = {
		.name = "VUL1",
		.id = MT8196_MEMIF_VUL1,
		.reg_ofs_base = AFE_VUL1_BASE,
		.reg_ofs_cur = AFE_VUL1_CUR,
		.reg_ofs_end = AFE_VUL1_END,
		.reg_ofs_base_msb = AFE_VUL1_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL1_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL1_END_MSB,
		.fs_reg = AFE_VUL1_CON0,
		.fs_shift = VUL1_SEL_FS_SFT,
		.fs_maskbit = VUL1_SEL_FS_MASK,
		.mono_reg = AFE_VUL1_CON0,
		.mono_shift = VUL1_MONO_SFT,
		.enable_reg = AFE_VUL1_CON0,
		.enable_shift = VUL1_ON_SFT,
		.hd_reg = AFE_VUL1_CON0,
		.hd_mask = VUL1_HD_MODE_MASK,
		.hd_shift = VUL1_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL1_CON0,
		.hd_align_mshift = VUL1_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL2] = {
		.name = "VUL2",
		.id = MT8196_MEMIF_VUL2,
		.reg_ofs_base = AFE_VUL2_BASE,
		.reg_ofs_cur = AFE_VUL2_CUR,
		.reg_ofs_end = AFE_VUL2_END,
		.reg_ofs_base_msb = AFE_VUL2_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL2_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL2_END_MSB,
		.fs_reg = AFE_VUL2_CON0,
		.fs_shift = VUL2_SEL_FS_SFT,
		.fs_maskbit = VUL2_SEL_FS_MASK,
		.mono_reg = AFE_VUL2_CON0,
		.mono_shift = VUL2_MONO_SFT,
		.enable_reg = AFE_VUL2_CON0,
		.enable_shift = VUL2_ON_SFT,
		.hd_reg = AFE_VUL2_CON0,
		.hd_mask = VUL2_HD_MODE_MASK,
		.hd_shift = VUL2_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL2_CON0,
		.hd_align_mshift = VUL2_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL3] = {
		.name = "VUL3",
		.id = MT8196_MEMIF_VUL3,
		.reg_ofs_base = AFE_VUL3_BASE,
		.reg_ofs_cur = AFE_VUL3_CUR,
		.reg_ofs_end = AFE_VUL3_END,
		.reg_ofs_base_msb = AFE_VUL3_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL3_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL3_END_MSB,
		.fs_reg = AFE_VUL3_CON0,
		.fs_shift = VUL3_SEL_FS_SFT,
		.fs_maskbit = VUL3_SEL_FS_MASK,
		.mono_reg = AFE_VUL3_CON0,
		.mono_shift = VUL3_MONO_SFT,
		.enable_reg = AFE_VUL3_CON0,
		.enable_shift = VUL3_ON_SFT,
		.hd_reg = AFE_VUL3_CON0,
		.hd_mask = VUL3_HD_MODE_MASK,
		.hd_shift = VUL3_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL3_CON0,
		.hd_align_mshift = VUL3_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL4] = {
		.name = "VUL4",
		.id = MT8196_MEMIF_VUL4,
		.reg_ofs_base = AFE_VUL4_BASE,
		.reg_ofs_cur = AFE_VUL4_CUR,
		.reg_ofs_end = AFE_VUL4_END,
		.reg_ofs_base_msb = AFE_VUL4_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL4_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL4_END_MSB,
		.fs_reg = AFE_VUL4_CON0,
		.fs_shift = VUL4_SEL_FS_SFT,
		.fs_maskbit = VUL4_SEL_FS_MASK,
		.mono_reg = AFE_VUL4_CON0,
		.mono_shift = VUL4_MONO_SFT,
		.enable_reg = AFE_VUL4_CON0,
		.enable_shift = VUL4_ON_SFT,
		.hd_reg = AFE_VUL4_CON0,
		.hd_mask = VUL4_HD_MODE_MASK,
		.hd_shift = VUL4_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL4_CON0,
		.hd_align_mshift = VUL4_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL5] = {
		.name = "VUL5",
		.id = MT8196_MEMIF_VUL5,
		.reg_ofs_base = AFE_VUL5_BASE,
		.reg_ofs_cur = AFE_VUL5_CUR,
		.reg_ofs_end = AFE_VUL5_END,
		.reg_ofs_base_msb = AFE_VUL5_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL5_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL5_END_MSB,
		.fs_reg = AFE_VUL5_CON0,
		.fs_shift = VUL5_SEL_FS_SFT,
		.fs_maskbit = VUL5_SEL_FS_MASK,
		.mono_reg = AFE_VUL5_CON0,
		.mono_shift = VUL5_MONO_SFT,
		.enable_reg = AFE_VUL5_CON0,
		.enable_shift = VUL5_ON_SFT,
		.hd_reg = AFE_VUL5_CON0,
		.hd_mask = VUL5_HD_MODE_MASK,
		.hd_shift = VUL5_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL5_CON0,
		.hd_align_mshift = VUL5_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL6] = {
		.name = "VUL6",
		.id = MT8196_MEMIF_VUL6,
		.reg_ofs_base = AFE_VUL6_BASE,
		.reg_ofs_cur = AFE_VUL6_CUR,
		.reg_ofs_end = AFE_VUL6_END,
		.reg_ofs_base_msb = AFE_VUL6_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL6_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL6_END_MSB,
		.fs_reg = AFE_VUL6_CON0,
		.fs_shift = VUL6_SEL_FS_SFT,
		.fs_maskbit = VUL6_SEL_FS_MASK,
		.mono_reg = AFE_VUL6_CON0,
		.mono_shift = VUL6_MONO_SFT,
		.enable_reg = AFE_VUL6_CON0,
		.enable_shift = VUL6_ON_SFT,
		.hd_reg = AFE_VUL6_CON0,
		.hd_mask = VUL6_HD_MODE_MASK,
		.hd_shift = VUL6_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL6_CON0,
		.hd_align_mshift = VUL6_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL7] = {
		.name = "VUL7",
		.id = MT8196_MEMIF_VUL7,
		.reg_ofs_base = AFE_VUL7_BASE,
		.reg_ofs_cur = AFE_VUL7_CUR,
		.reg_ofs_end = AFE_VUL7_END,
		.reg_ofs_base_msb = AFE_VUL7_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL7_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL7_END_MSB,
		.fs_reg = AFE_VUL7_CON0,
		.fs_shift = VUL7_SEL_FS_SFT,
		.fs_maskbit = VUL7_SEL_FS_MASK,
		.mono_reg = AFE_VUL7_CON0,
		.mono_shift = VUL7_MONO_SFT,
		.enable_reg = AFE_VUL7_CON0,
		.enable_shift = VUL7_ON_SFT,
		.hd_reg = AFE_VUL7_CON0,
		.hd_mask = VUL7_HD_MODE_MASK,
		.hd_shift = VUL7_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL7_CON0,
		.hd_align_mshift = VUL7_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL8] = {
		.name = "VUL8",
		.id = MT8196_MEMIF_VUL8,
		.reg_ofs_base = AFE_VUL8_BASE,
		.reg_ofs_cur = AFE_VUL8_CUR,
		.reg_ofs_end = AFE_VUL8_END,
		.reg_ofs_base_msb = AFE_VUL8_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL8_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL8_END_MSB,
		.fs_reg = AFE_VUL8_CON0,
		.fs_shift = VUL8_SEL_FS_SFT,
		.fs_maskbit = VUL8_SEL_FS_MASK,
		.mono_reg = AFE_VUL8_CON0,
		.mono_shift = VUL8_MONO_SFT,
		.enable_reg = AFE_VUL8_CON0,
		.enable_shift = VUL8_ON_SFT,
		.hd_reg = AFE_VUL8_CON0,
		.hd_mask = VUL8_HD_MODE_MASK,
		.hd_shift = VUL8_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL8_CON0,
		.hd_align_mshift = VUL8_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL9] = {
		.name = "VUL9",
		.id = MT8196_MEMIF_VUL9,
		.reg_ofs_base = AFE_VUL9_BASE,
		.reg_ofs_cur = AFE_VUL9_CUR,
		.reg_ofs_end = AFE_VUL9_END,
		.reg_ofs_base_msb = AFE_VUL9_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL9_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL9_END_MSB,
		.fs_reg = AFE_VUL9_CON0,
		.fs_shift = VUL9_SEL_FS_SFT,
		.fs_maskbit = VUL9_SEL_FS_MASK,
		.mono_reg = AFE_VUL9_CON0,
		.mono_shift = VUL9_MONO_SFT,
		.enable_reg = AFE_VUL9_CON0,
		.enable_shift = VUL9_ON_SFT,
		.hd_reg = AFE_VUL9_CON0,
		.hd_mask = VUL9_HD_MODE_MASK,
		.hd_shift = VUL9_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL9_CON0,
		.hd_align_mshift = VUL9_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL10] = {
		.name = "VUL10",
		.id = MT8196_MEMIF_VUL10,
		.reg_ofs_base = AFE_VUL10_BASE,
		.reg_ofs_cur = AFE_VUL10_CUR,
		.reg_ofs_end = AFE_VUL10_END,
		.reg_ofs_base_msb = AFE_VUL10_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL10_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL10_END_MSB,
		.fs_reg = AFE_VUL10_CON0,
		.fs_shift = VUL10_SEL_FS_SFT,
		.fs_maskbit = VUL10_SEL_FS_MASK,
		.mono_reg = AFE_VUL10_CON0,
		.mono_shift = VUL10_MONO_SFT,
		.enable_reg = AFE_VUL10_CON0,
		.enable_shift = VUL10_ON_SFT,
		.hd_reg = AFE_VUL10_CON0,
		.hd_mask = VUL10_HD_MODE_MASK,
		.hd_shift = VUL10_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL10_CON0,
		.hd_align_mshift = VUL10_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL24] = {
		.name = "VUL24",
		.id = MT8196_MEMIF_VUL24,
		.reg_ofs_base = AFE_VUL24_BASE,
		.reg_ofs_cur = AFE_VUL24_CUR,
		.reg_ofs_end = AFE_VUL24_END,
		.reg_ofs_base_msb = AFE_VUL24_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL24_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL24_END_MSB,
		.fs_reg = AFE_VUL24_CON0,
		.fs_shift = VUL24_SEL_FS_SFT,
		.fs_maskbit = VUL24_SEL_FS_MASK,
		.mono_reg = AFE_VUL24_CON0,
		.mono_shift = VUL24_MONO_SFT,
		.enable_reg = AFE_VUL24_CON0,
		.enable_shift = VUL24_ON_SFT,
		.hd_reg = AFE_VUL24_CON0,
		.hd_mask = VUL24_HD_MODE_MASK,
		.hd_shift = VUL24_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL24_CON0,
		.hd_align_mshift = VUL24_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.out_on_use_reg = AFE_VUL24_CON0,
		.out_on_use_mask = OUT_ON_USE_VUL24_MASK,
		.out_on_use_shift = OUT_ON_USE_VUL24_SFT,
	},
	[MT8196_MEMIF_VUL25] = {
		.name = "VUL25",
		.id = MT8196_MEMIF_VUL25,
		.reg_ofs_base = AFE_VUL25_BASE,
		.reg_ofs_cur = AFE_VUL25_CUR,
		.reg_ofs_end = AFE_VUL25_END,
		.reg_ofs_base_msb = AFE_VUL25_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL25_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL25_END_MSB,
		.fs_reg = AFE_VUL25_CON0,
		.fs_shift = VUL25_SEL_FS_SFT,
		.fs_maskbit = VUL25_SEL_FS_MASK,
		.mono_reg = AFE_VUL25_CON0,
		.mono_shift = VUL25_MONO_SFT,
		.enable_reg = AFE_VUL25_CON0,
		.enable_shift = VUL25_ON_SFT,
		.hd_reg = AFE_VUL25_CON0,
		.hd_mask = VUL25_HD_MODE_MASK,
		.hd_shift = VUL25_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL25_CON0,
		.hd_align_mshift = VUL25_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.out_on_use_reg = AFE_VUL25_CON0,
		.out_on_use_mask = OUT_ON_USE_VUL25_MASK,
		.out_on_use_shift = OUT_ON_USE_VUL25_SFT,
	},
	[MT8196_MEMIF_VUL26] = {
		.name = "VUL26",
		.id = MT8196_MEMIF_VUL26,
		.reg_ofs_base = AFE_VUL26_BASE,
		.reg_ofs_cur = AFE_VUL26_CUR,
		.reg_ofs_end = AFE_VUL26_END,
		.reg_ofs_base_msb = AFE_VUL26_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL26_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL26_END_MSB,
		.fs_reg = AFE_VUL26_CON0,
		.fs_shift = VUL26_SEL_FS_SFT,
		.fs_maskbit = VUL26_SEL_FS_MASK,
		.mono_reg = AFE_VUL26_CON0,
		.mono_shift = VUL26_MONO_SFT,
		.enable_reg = AFE_VUL26_CON0,
		.enable_shift = VUL26_ON_SFT,
		.hd_reg = AFE_VUL26_CON0,
		.hd_mask = VUL26_HD_MODE_MASK,
		.hd_shift = VUL26_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL26_CON0,
		.hd_align_mshift = VUL26_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.out_on_use_reg = AFE_VUL26_CON0,
		.out_on_use_mask = OUT_ON_USE_VUL26_MASK,
		.out_on_use_shift = OUT_ON_USE_VUL26_SFT,
	},
	[MT8196_MEMIF_VUL_CM0] = {
		.name = "VUL_CM0",
		.id = MT8196_MEMIF_VUL_CM0,
		.reg_ofs_base = AFE_VUL_CM0_BASE,
		.reg_ofs_cur = AFE_VUL_CM0_CUR,
		.reg_ofs_end = AFE_VUL_CM0_END,
		.reg_ofs_base_msb = AFE_VUL_CM0_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL_CM0_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL_CM0_END_MSB,
		.enable_reg = AFE_VUL_CM0_CON0,
		.enable_shift = VUL_CM0_ON_SFT,
		.hd_reg = AFE_VUL_CM0_CON0,
		.hd_mask = VUL_CM0_HD_MODE_MASK,
		.hd_shift = VUL_CM0_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL_CM0_CON0,
		.hd_align_mshift = VUL_CM0_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL_CM1] = {
		.name = "VUL_CM1",
		.id = MT8196_MEMIF_VUL_CM1,
		.reg_ofs_base = AFE_VUL_CM1_BASE,
		.reg_ofs_cur = AFE_VUL_CM1_CUR,
		.reg_ofs_end = AFE_VUL_CM1_END,
		.reg_ofs_base_msb = AFE_VUL_CM1_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL_CM1_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL_CM1_END_MSB,
		.enable_reg = AFE_VUL_CM1_CON0,
		.enable_shift = VUL_CM1_ON_SFT,
		.hd_reg = AFE_VUL_CM1_CON0,
		.hd_mask = VUL_CM1_HD_MODE_MASK,
		.hd_shift = VUL_CM1_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL_CM1_CON0,
		.hd_align_mshift = VUL_CM1_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_VUL_CM2] = {
		.name = "VUL_CM2",
		.id = MT8196_MEMIF_VUL_CM2,
		.reg_ofs_base = AFE_VUL_CM2_BASE,
		.reg_ofs_cur = AFE_VUL_CM2_CUR,
		.reg_ofs_end = AFE_VUL_CM2_END,
		.reg_ofs_base_msb = AFE_VUL_CM2_BASE_MSB,
		.reg_ofs_cur_msb = AFE_VUL_CM2_CUR_MSB,
		.reg_ofs_end_msb = AFE_VUL_CM2_END_MSB,
		.enable_reg = AFE_VUL_CM2_CON0,
		.enable_shift = VUL_CM2_ON_SFT,
		.hd_reg = AFE_VUL_CM2_CON0,
		.hd_mask = VUL_CM2_HD_MODE_MASK,
		.hd_shift = VUL_CM2_HD_MODE_SFT,
		.hd_align_reg = AFE_VUL_CM2_CON0,
		.hd_align_mshift = VUL_CM2_HALIGN_SFT,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_ETDM_IN0] = {
		.name = "ETDM_IN0",
		.id = MT8196_MEMIF_ETDM_IN0,
		.reg_ofs_base = AFE_ETDM_IN0_BASE,
		.reg_ofs_cur = AFE_ETDM_IN0_CUR,
		.reg_ofs_end = AFE_ETDM_IN0_END,
		.reg_ofs_base_msb = AFE_ETDM_IN0_BASE_MSB,
		.reg_ofs_cur_msb = AFE_ETDM_IN0_CUR_MSB,
		.reg_ofs_end_msb = AFE_ETDM_IN0_END_MSB,
		.fs_reg = ETDM_IN0_CON3,
		.fs_shift = REG_FS_TIMING_SEL_SFT,
		.fs_maskbit = REG_FS_TIMING_SEL_MASK,
		.enable_reg = AFE_ETDM_IN0_CON0,
		.enable_shift = ETDM_IN0_ON_SFT,
		.hd_reg = AFE_ETDM_IN0_CON0,
		.hd_mask = ETDM_IN0_HD_MODE_MASK,
		.hd_shift = ETDM_IN0_HD_MODE_SFT,
		.hd_align_reg = AFE_ETDM_IN0_CON0,
		.hd_align_mshift = ETDM_IN0_HALIGN_SFT,
		.hd_msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_ETDM_IN1] = {
		.name = "ETDM_IN1",
		.id = MT8196_MEMIF_ETDM_IN1,
		.reg_ofs_base = AFE_ETDM_IN1_BASE,
		.reg_ofs_cur = AFE_ETDM_IN1_CUR,
		.reg_ofs_end = AFE_ETDM_IN1_END,
		.reg_ofs_base_msb = AFE_ETDM_IN1_BASE_MSB,
		.reg_ofs_cur_msb = AFE_ETDM_IN1_CUR_MSB,
		.reg_ofs_end_msb = AFE_ETDM_IN1_END_MSB,
		.fs_reg = ETDM_IN1_CON3,
		.fs_shift = REG_FS_TIMING_SEL_SFT,
		.fs_maskbit = REG_FS_TIMING_SEL_MASK,
		.enable_reg = AFE_ETDM_IN1_CON0,
		.enable_shift = ETDM_IN1_ON_SFT,
		.hd_reg = AFE_ETDM_IN1_CON0,
		.hd_mask = ETDM_IN1_HD_MODE_MASK,
		.hd_shift = ETDM_IN1_HD_MODE_SFT,
		.hd_align_reg = AFE_ETDM_IN1_CON0,
		.hd_align_mshift = ETDM_IN1_HALIGN_SFT,
		.hd_msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_ETDM_IN2] = {
		.name = "ETDM_IN2",
		.id = MT8196_MEMIF_ETDM_IN2,
		.reg_ofs_base = AFE_ETDM_IN2_BASE,
		.reg_ofs_cur = AFE_ETDM_IN2_CUR,
		.reg_ofs_end = AFE_ETDM_IN2_END,
		.reg_ofs_base_msb = AFE_ETDM_IN2_BASE_MSB,
		.reg_ofs_cur_msb = AFE_ETDM_IN2_CUR_MSB,
		.reg_ofs_end_msb = AFE_ETDM_IN2_END_MSB,
		.fs_reg = ETDM_IN2_CON3,
		.fs_shift = REG_FS_TIMING_SEL_SFT,
		.fs_maskbit = REG_FS_TIMING_SEL_MASK,
		.enable_reg = AFE_ETDM_IN2_CON0,
		.enable_shift = ETDM_IN2_ON_SFT,
		.hd_reg = AFE_ETDM_IN2_CON0,
		.hd_mask = ETDM_IN2_HD_MODE_MASK,
		.hd_shift = ETDM_IN2_HD_MODE_SFT,
		.hd_align_reg = AFE_ETDM_IN2_CON0,
		.hd_align_mshift = ETDM_IN2_HALIGN_SFT,
		.hd_msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_ETDM_IN3] = {
		.name = "ETDM_IN3",
		.id = MT8196_MEMIF_ETDM_IN3,
		.reg_ofs_base = AFE_ETDM_IN3_BASE,
		.reg_ofs_cur = AFE_ETDM_IN3_CUR,
		.reg_ofs_end = AFE_ETDM_IN3_END,
		.reg_ofs_base_msb = AFE_ETDM_IN3_BASE_MSB,
		.reg_ofs_cur_msb = AFE_ETDM_IN3_CUR_MSB,
		.reg_ofs_end_msb = AFE_ETDM_IN3_END_MSB,
		.fs_reg = ETDM_IN3_CON3,
		.fs_shift = REG_FS_TIMING_SEL_SFT,
		.fs_maskbit = REG_FS_TIMING_SEL_MASK,
		.enable_reg = AFE_ETDM_IN3_CON0,
		.enable_shift = ETDM_IN3_ON_SFT,
		.hd_reg = AFE_ETDM_IN3_CON0,
		.hd_mask = ETDM_IN3_HD_MODE_MASK,
		.hd_shift = ETDM_IN3_HD_MODE_SFT,
		.hd_align_reg = AFE_ETDM_IN3_CON0,
		.hd_align_mshift = ETDM_IN3_HALIGN_SFT,
		.hd_msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_ETDM_IN4] = {
		.name = "ETDM_IN4",
		.id = MT8196_MEMIF_ETDM_IN4,
		.reg_ofs_base = AFE_ETDM_IN4_BASE,
		.reg_ofs_cur = AFE_ETDM_IN4_CUR,
		.reg_ofs_end = AFE_ETDM_IN4_END,
		.reg_ofs_base_msb = AFE_ETDM_IN4_BASE_MSB,
		.reg_ofs_cur_msb = AFE_ETDM_IN4_CUR_MSB,
		.reg_ofs_end_msb = AFE_ETDM_IN4_END_MSB,
		.fs_reg = ETDM_IN4_CON3,
		.fs_shift = REG_FS_TIMING_SEL_SFT,
		.fs_maskbit = REG_FS_TIMING_SEL_MASK,
		.enable_reg = AFE_ETDM_IN4_CON0,
		.enable_shift = ETDM_IN4_ON_SFT,
		.hd_reg = AFE_ETDM_IN4_CON0,
		.hd_mask = ETDM_IN4_HD_MODE_MASK,
		.hd_shift = ETDM_IN4_HD_MODE_SFT,
		.hd_align_reg = AFE_ETDM_IN4_CON0,
		.hd_align_mshift = ETDM_IN4_HALIGN_SFT,
		.hd_msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_ETDM_IN6] = {
		.name = "ETDM_IN6",
		.id = MT8196_MEMIF_ETDM_IN6,
		.reg_ofs_base = AFE_ETDM_IN6_BASE,
		.reg_ofs_cur = AFE_ETDM_IN6_CUR,
		.reg_ofs_end = AFE_ETDM_IN6_END,
		.reg_ofs_base_msb = AFE_ETDM_IN6_BASE_MSB,
		.reg_ofs_cur_msb = AFE_ETDM_IN6_CUR_MSB,
		.reg_ofs_end_msb = AFE_ETDM_IN6_END_MSB,
		.fs_reg = ETDM_IN6_CON3,
		.fs_shift = REG_FS_TIMING_SEL_SFT,
		.fs_maskbit = REG_FS_TIMING_SEL_MASK,
		.enable_reg = AFE_ETDM_IN6_CON0,
		.enable_shift = ETDM_IN6_ON_SFT,
		.hd_reg = AFE_ETDM_IN6_CON0,
		.hd_mask = ETDM_IN6_HD_MODE_MASK,
		.hd_shift = ETDM_IN6_HD_MODE_SFT,
		.hd_align_reg = AFE_ETDM_IN6_CON0,
		.hd_align_mshift = ETDM_IN6_HALIGN_SFT,
		.hd_msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
	},
	[MT8196_MEMIF_HDMI] = {
		.name = "HDMI",
		.id = MT8196_MEMIF_HDMI,
		.reg_ofs_base = AFE_HDMI_OUT_BASE,
		.reg_ofs_cur = AFE_HDMI_OUT_CUR,
		.reg_ofs_end = AFE_HDMI_OUT_END,
		.reg_ofs_base_msb = AFE_HDMI_OUT_BASE_MSB,
		.reg_ofs_cur_msb = AFE_HDMI_OUT_CUR_MSB,
		.reg_ofs_end_msb = AFE_HDMI_OUT_END_MSB,
		.fs_reg = -1,
		.fs_shift = -1,
		.fs_maskbit = -1,
		.mono_reg = -1,
		.mono_shift = -1,
		.enable_reg = AFE_HDMI_OUT_CON0,
		.enable_shift = HDMI_OUT_ON_SFT,
		.hd_reg = AFE_HDMI_OUT_CON0,
		.hd_mask = HDMI_OUT_HD_MODE_MASK,
		.hd_shift = HDMI_OUT_HD_MODE_SFT,
		.hd_align_reg = AFE_HDMI_OUT_CON0,
		.hd_align_mshift = HDMI_OUT_HALIGN_SFT,
		.hd_msb_shift = -1,
		.agent_disable_reg = -1,
		.agent_disable_shift = -1,
		.msb_reg = -1,
		.msb_shift = -1,
		.pbuf_reg = AFE_HDMI_OUT_CON0,
		.pbuf_mask = HDMI_OUT_PBUF_SIZE_MASK,
		.pbuf_shift = HDMI_OUT_PBUF_SIZE_SFT,
		.minlen_reg = AFE_HDMI_OUT_CON0,
		.minlen_mask = HDMI_OUT_MINLEN_MASK,
		.minlen_shift = HDMI_OUT_MINLEN_SFT,
	},
};

static const struct mtk_base_irq_data irq_data[MT8196_IRQ_NUM] = {
	[MT8196_IRQ_0] = {
		.id = MT8196_IRQ_0,
		.irq_cnt_reg = AFE_IRQ0_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ0_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ0_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ0_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ0_MCU_CFG0,
		.irq_en_shift = AFE_IRQ0_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ0_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ0_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ0_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_1] = {
		.id = MT8196_IRQ_1,
		.irq_cnt_reg = AFE_IRQ1_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ1_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ1_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ1_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ1_MCU_CFG0,
		.irq_en_shift = AFE_IRQ1_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ1_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ1_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ1_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_2] = {
		.id = MT8196_IRQ_2,
		.irq_cnt_reg = AFE_IRQ2_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ2_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ2_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ2_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ2_MCU_CFG0,
		.irq_en_shift = AFE_IRQ2_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ2_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ2_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ2_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_3] = {
		.id = MT8196_IRQ_3,
		.irq_cnt_reg = AFE_IRQ3_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ3_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ3_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ3_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ3_MCU_CFG0,
		.irq_en_shift = AFE_IRQ3_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ3_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ3_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ3_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_4] = {
		.id = MT8196_IRQ_4,
		.irq_cnt_reg = AFE_IRQ4_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ4_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ4_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ4_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ4_MCU_CFG0,
		.irq_en_shift = AFE_IRQ4_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ4_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ4_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ4_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_5] = {
		.id = MT8196_IRQ_5,
		.irq_cnt_reg = AFE_IRQ5_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ5_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ5_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ5_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ5_MCU_CFG0,
		.irq_en_shift = AFE_IRQ5_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ5_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ5_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ5_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_6] = {
		.id = MT8196_IRQ_6,
		.irq_cnt_reg = AFE_IRQ6_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ6_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ6_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ6_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ6_MCU_CFG0,
		.irq_en_shift = AFE_IRQ6_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ6_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ6_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ6_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_7] = {
		.id = MT8196_IRQ_7,
		.irq_cnt_reg = AFE_IRQ7_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ7_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ7_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ7_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ7_MCU_CFG0,
		.irq_en_shift = AFE_IRQ7_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ7_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ7_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ7_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_8] = {
		.id = MT8196_IRQ_8,
		.irq_cnt_reg = AFE_IRQ8_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ8_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ8_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ8_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ8_MCU_CFG0,
		.irq_en_shift = AFE_IRQ8_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ8_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ8_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ8_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_9] = {
		.id = MT8196_IRQ_9,
		.irq_cnt_reg = AFE_IRQ9_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ9_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ9_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ9_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ9_MCU_CFG0,
		.irq_en_shift = AFE_IRQ9_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ9_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ9_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ9_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_10] = {
		.id = MT8196_IRQ_10,
		.irq_cnt_reg = AFE_IRQ10_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ10_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ10_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ10_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ10_MCU_CFG0,
		.irq_en_shift = AFE_IRQ10_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ10_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ10_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ10_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_11] = {
		.id = MT8196_IRQ_11,
		.irq_cnt_reg = AFE_IRQ11_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ11_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ11_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ11_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ11_MCU_CFG0,
		.irq_en_shift = AFE_IRQ11_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ11_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ11_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ11_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_12] = {
		.id = MT8196_IRQ_12,
		.irq_cnt_reg = AFE_IRQ12_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ12_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ12_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ12_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ12_MCU_CFG0,
		.irq_en_shift = AFE_IRQ12_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ12_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ12_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ12_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_13] = {
		.id = MT8196_IRQ_13,
		.irq_cnt_reg = AFE_IRQ13_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ13_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ13_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ13_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ13_MCU_CFG0,
		.irq_en_shift = AFE_IRQ13_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ13_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ13_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ13_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_14] = {
		.id = MT8196_IRQ_14,
		.irq_cnt_reg = AFE_IRQ14_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ14_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ14_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ14_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ14_MCU_CFG0,
		.irq_en_shift = AFE_IRQ14_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ14_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ14_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ14_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_15] = {
		.id = MT8196_IRQ_15,
		.irq_cnt_reg = AFE_IRQ15_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ15_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ15_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ15_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ15_MCU_CFG0,
		.irq_en_shift = AFE_IRQ15_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ15_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ15_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ15_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_16] = {
		.id = MT8196_IRQ_16,
		.irq_cnt_reg = AFE_IRQ16_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ16_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ16_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ16_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ16_MCU_CFG0,
		.irq_en_shift = AFE_IRQ16_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ16_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ16_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ16_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_17] = {
		.id = MT8196_IRQ_17,
		.irq_cnt_reg = AFE_IRQ17_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ17_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ17_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ17_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ17_MCU_CFG0,
		.irq_en_shift = AFE_IRQ17_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ17_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ17_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ17_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_18] = {
		.id = MT8196_IRQ_18,
		.irq_cnt_reg = AFE_IRQ18_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ18_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ18_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ18_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ18_MCU_CFG0,
		.irq_en_shift = AFE_IRQ18_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ18_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ18_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ18_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_19] = {
		.id = MT8196_IRQ_19,
		.irq_cnt_reg = AFE_IRQ19_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ19_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ19_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ19_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ19_MCU_CFG0,
		.irq_en_shift = AFE_IRQ19_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ19_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ19_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ19_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_20] = {
		.id = MT8196_IRQ_20,
		.irq_cnt_reg = AFE_IRQ20_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ20_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ20_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ20_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ20_MCU_CFG0,
		.irq_en_shift = AFE_IRQ20_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ20_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ20_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ20_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_21] = {
		.id = MT8196_IRQ_21,
		.irq_cnt_reg = AFE_IRQ21_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ21_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ21_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ21_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ21_MCU_CFG0,
		.irq_en_shift = AFE_IRQ21_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ21_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ21_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ21_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_22] = {
		.id = MT8196_IRQ_22,
		.irq_cnt_reg = AFE_IRQ22_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ22_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ22_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ22_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ22_MCU_CFG0,
		.irq_en_shift = AFE_IRQ22_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ22_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ22_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ22_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_23] = {
		.id = MT8196_IRQ_23,
		.irq_cnt_reg = AFE_IRQ23_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ23_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ23_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ23_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ23_MCU_CFG0,
		.irq_en_shift = AFE_IRQ23_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ23_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ23_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ23_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_24] = {
		.id = MT8196_IRQ_24,
		.irq_cnt_reg = AFE_IRQ24_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ24_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ24_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ24_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ24_MCU_CFG0,
		.irq_en_shift = AFE_IRQ24_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ24_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ24_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ24_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_25] = {
		.id = MT8196_IRQ_25,
		.irq_cnt_reg = AFE_IRQ25_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ25_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ25_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ25_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ25_MCU_CFG0,
		.irq_en_shift = AFE_IRQ25_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ25_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ25_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ25_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_26] = {
		.id = MT8196_IRQ_26,
		.irq_cnt_reg = AFE_IRQ26_MCU_CFG1,
		.irq_cnt_shift = AFE_IRQ_CNT_SHIFT,
		.irq_cnt_maskbit = AFE_IRQ_CNT_MASK,
		.irq_fs_reg = AFE_IRQ26_MCU_CFG0,
		.irq_fs_shift = AFE_IRQ26_MCU_FS_SFT,
		.irq_fs_maskbit = AFE_IRQ26_MCU_FS_MASK,
		.irq_en_reg = AFE_IRQ26_MCU_CFG0,
		.irq_en_shift = AFE_IRQ26_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ26_MCU_CFG1,
		.irq_clr_shift = AFE_IRQ26_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_IRQ_MCU_SCP_EN,
		.irq_scp_en_shift = IRQ26_MCU_SCP_EN_SFT,
	},
	[MT8196_IRQ_31] = {
		.id = MT8196_CUS_IRQ_TDM,
		.irq_cnt_reg = AFE_CUSTOM_IRQ0_MCU_CFG1,
		.irq_cnt_shift = AFE_CUSTOM_IRQ0_MCU_CNT_SFT,
		.irq_cnt_maskbit = AFE_CUSTOM_IRQ0_MCU_CNT_MASK,
		.irq_fs_reg = -1,
		.irq_fs_shift = -1,
		.irq_fs_maskbit = -1,
		.irq_en_reg = AFE_CUSTOM_IRQ0_MCU_CFG0,
		.irq_en_shift = AFE_CUSTOM_IRQ0_MCU_ON_SFT,
		.irq_clr_reg = AFE_CUSTOM_IRQ0_MCU_CFG1,
		.irq_clr_shift = AFE_CUSTOM_IRQ0_CLR_CFG_SFT,
		.irq_ap_en_reg = AFE_CUSTOM_IRQ_MCU_EN,
		.irq_scp_en_reg = AFE_CUSTOM_IRQ_MCU_SCP_EN,
	},
};

static const int memif_irq_usage[MT8196_MEMIF_NUM] = {
	/* TODO: verify each memif & irq */
	[MT8196_MEMIF_DL0] = MT8196_IRQ_0,
	[MT8196_MEMIF_DL1] = MT8196_IRQ_1,
	[MT8196_MEMIF_DL2] = MT8196_IRQ_2,
	[MT8196_MEMIF_DL3] = MT8196_IRQ_3,
	[MT8196_MEMIF_DL4] = MT8196_IRQ_4,
	[MT8196_MEMIF_DL5] = MT8196_IRQ_5,
	[MT8196_MEMIF_DL6] = MT8196_IRQ_6,
	[MT8196_MEMIF_DL7] = MT8196_IRQ_7,
	[MT8196_MEMIF_DL8] = MT8196_IRQ_8,
	[MT8196_MEMIF_DL23] = MT8196_IRQ_9,
	[MT8196_MEMIF_DL24] = MT8196_IRQ_10,
	[MT8196_MEMIF_DL25] = MT8196_IRQ_11,
	[MT8196_MEMIF_DL26] = MT8196_IRQ_0,
	[MT8196_MEMIF_DL_4CH] = MT8196_IRQ_0,
	[MT8196_MEMIF_DL_24CH] = MT8196_IRQ_12,
	[MT8196_MEMIF_VUL0] = MT8196_IRQ_13,
	[MT8196_MEMIF_VUL1] = MT8196_IRQ_14,
	[MT8196_MEMIF_VUL2] = MT8196_IRQ_15,
	[MT8196_MEMIF_VUL3] = MT8196_IRQ_16,
	[MT8196_MEMIF_VUL4] = MT8196_IRQ_17,
	[MT8196_MEMIF_VUL5] = MT8196_IRQ_18,
	[MT8196_MEMIF_VUL6] = MT8196_IRQ_19,
	[MT8196_MEMIF_VUL7] = MT8196_IRQ_20,
	[MT8196_MEMIF_VUL8] = MT8196_IRQ_21,
	[MT8196_MEMIF_VUL9] = MT8196_IRQ_22,
	[MT8196_MEMIF_VUL10] = MT8196_IRQ_23,
	[MT8196_MEMIF_VUL24] = MT8196_IRQ_24,
	[MT8196_MEMIF_VUL25] = MT8196_IRQ_25,
	[MT8196_MEMIF_VUL26] = MT8196_IRQ_0,
	[MT8196_MEMIF_VUL_CM0] = MT8196_IRQ_26,
	[MT8196_MEMIF_VUL_CM1] = MT8196_IRQ_0,
	[MT8196_MEMIF_VUL_CM2] = MT8196_IRQ_0,
	[MT8196_MEMIF_ETDM_IN0] = MT8196_IRQ_0,
	[MT8196_MEMIF_ETDM_IN1] = MT8196_IRQ_0,
	[MT8196_MEMIF_ETDM_IN2] = MT8196_IRQ_0,
	[MT8196_MEMIF_ETDM_IN3] = MT8196_IRQ_0,
	[MT8196_MEMIF_ETDM_IN4] = MT8196_IRQ_0,
	[MT8196_MEMIF_ETDM_IN6] = MT8196_IRQ_0,
	[MT8196_MEMIF_HDMI] = MT8196_IRQ_31
};

static bool mt8196_is_volatile_reg(struct device *dev, unsigned int reg)
{
	/* these auto-gen reg has read-only bit, so put it as volatile */
	/* volatile reg cannot be cached, so cannot be set when power off */
	switch (reg) {
	case AUDIO_TOP_CON0:    /* reg bit controlled by CCF */
	case AUDIO_TOP_CON1:    /* reg bit controlled by CCF */
	case AUDIO_TOP_CON2:
	case AUDIO_TOP_CON3:
	case AUDIO_TOP_CON4:
	case AUD_TOP_MON_RG:
	case AFE_APLL1_TUNER_MON0:
	case AFE_APLL2_TUNER_MON0:
	case AFE_SPM_CONTROL_ACK:
	case AUDIO_TOP_IP_VERSION:
	case AUDIO_ENGEN_CON0_MON:
	case AFE_CONNSYS_I2S_IPM_VER_MON:
	case AFE_CONNSYS_I2S_MON:
	case AFE_PCM_INTF_MON:
	case AFE_PCM_TOP_IP_VERSION:
	case AFE_IRQ_MCU_STATUS:
	case AFE_CUSTOM_IRQ_MCU_STATUS:
	case AFE_IRQ_MCU_MON0:
	case AFE_IRQ_MCU_MON1:
	case AFE_IRQ_MCU_MON2:
	case AFE_IRQ0_CNT_MON:
	case AFE_IRQ1_CNT_MON:
	case AFE_IRQ2_CNT_MON:
	case AFE_IRQ3_CNT_MON:
	case AFE_IRQ4_CNT_MON:
	case AFE_IRQ5_CNT_MON:
	case AFE_IRQ6_CNT_MON:
	case AFE_IRQ7_CNT_MON:
	case AFE_IRQ8_CNT_MON:
	case AFE_IRQ9_CNT_MON:
	case AFE_IRQ10_CNT_MON:
	case AFE_IRQ11_CNT_MON:
	case AFE_IRQ12_CNT_MON:
	case AFE_IRQ13_CNT_MON:
	case AFE_IRQ14_CNT_MON:
	case AFE_IRQ15_CNT_MON:
	case AFE_IRQ16_CNT_MON:
	case AFE_IRQ17_CNT_MON:
	case AFE_IRQ18_CNT_MON:
	case AFE_IRQ19_CNT_MON:
	case AFE_IRQ20_CNT_MON:
	case AFE_IRQ21_CNT_MON:
	case AFE_IRQ22_CNT_MON:
	case AFE_IRQ23_CNT_MON:
	case AFE_IRQ24_CNT_MON:
	case AFE_IRQ25_CNT_MON:
	case AFE_IRQ26_CNT_MON:
	case AFE_CUSTOM_IRQ0_CNT_MON:
	case AFE_STF_MON:
	case AFE_STF_IP_VERSION:
	case AFE_CM0_MON:
	case AFE_CM0_IP_VERSION:
	case AFE_CM1_MON:
	case AFE_CM1_IP_VERSION:
	case AFE_ADDA_UL0_SRC_DEBUG_MON0:
	case AFE_ADDA_UL0_SRC_MON0:
	case AFE_ADDA_UL0_SRC_MON1:
	case AFE_ADDA_UL0_IP_VERSION:
	case AFE_ADDA_UL1_SRC_DEBUG_MON0:
	case AFE_ADDA_UL1_SRC_MON0:
	case AFE_ADDA_UL1_SRC_MON1:
	case AFE_ADDA_UL1_IP_VERSION:
	case AFE_MTKAIF_IPM_VER_MON:
	case AFE_MTKAIF_MON:
	case AFE_AUD_PAD_TOP_MON:
	case AFE_ADDA_MTKAIFV4_MON0:
	case AFE_ADDA_MTKAIFV4_MON1:
	case AFE_ADDA6_MTKAIFV4_MON0:
	case ETDM_IN0_MON:
	case ETDM_IN1_MON:
	case ETDM_IN2_MON:
	case ETDM_IN4_MON:
	case ETDM_IN6_MON:
	case ETDM_OUT0_MON:
	case ETDM_OUT1_MON:
	case ETDM_OUT2_MON:
	case ETDM_OUT4_MON:
	case ETDM_OUT6_MON:
	case AFE_DPTX_MON:
	case AFE_TDM_TOP_IP_VERSION:
	case AFE_CONN_MON0:
	case AFE_CONN_MON1:
	case AFE_CONN_MON2:
	case AFE_CONN_MON3:
	case AFE_CONN_MON4:
	case AFE_CONN_MON5:
	case AFE_CBIP_SLV_DECODER_MON0:
	case AFE_CBIP_SLV_DECODER_MON1:
	case AFE_CBIP_SLV_MUX_MON0:
	case AFE_CBIP_SLV_MUX_MON1:
	case AFE_DL0_CUR_MSB:
	case AFE_DL0_CUR:
	case AFE_DL0_RCH_MON:
	case AFE_DL0_LCH_MON:
	case AFE_DL1_CUR_MSB:
	case AFE_DL1_CUR:
	case AFE_DL1_RCH_MON:
	case AFE_DL1_LCH_MON:
	case AFE_DL2_CUR_MSB:
	case AFE_DL2_CUR:
	case AFE_DL2_RCH_MON:
	case AFE_DL2_LCH_MON:
	case AFE_DL3_CUR_MSB:
	case AFE_DL3_CUR:
	case AFE_DL3_RCH_MON:
	case AFE_DL3_LCH_MON:
	case AFE_DL4_CUR_MSB:
	case AFE_DL4_CUR:
	case AFE_DL4_RCH_MON:
	case AFE_DL4_LCH_MON:
	case AFE_DL5_CUR_MSB:
	case AFE_DL5_CUR:
	case AFE_DL5_RCH_MON:
	case AFE_DL5_LCH_MON:
	case AFE_DL6_CUR_MSB:
	case AFE_DL6_CUR:
	case AFE_DL6_RCH_MON:
	case AFE_DL6_LCH_MON:
	case AFE_DL7_CUR_MSB:
	case AFE_DL7_CUR:
	case AFE_DL7_RCH_MON:
	case AFE_DL7_LCH_MON:
	case AFE_DL8_CUR_MSB:
	case AFE_DL8_CUR:
	case AFE_DL8_RCH_MON:
	case AFE_DL8_LCH_MON:
	case AFE_DL_24CH_CUR_MSB:
	case AFE_DL_24CH_CUR:
	case AFE_DL_4CH_CUR_MSB:
	case AFE_DL_4CH_CUR:
	case AFE_DL23_CUR_MSB:
	case AFE_DL23_CUR:
	case AFE_DL23_RCH_MON:
	case AFE_DL23_LCH_MON:
	case AFE_DL24_CUR_MSB:
	case AFE_DL24_CUR:
	case AFE_DL24_RCH_MON:
	case AFE_DL24_LCH_MON:
	case AFE_DL25_CUR_MSB:
	case AFE_DL25_CUR:
	case AFE_DL25_RCH_MON:
	case AFE_DL25_LCH_MON:
	case AFE_DL26_CUR_MSB:
	case AFE_DL26_CUR:
	case AFE_DL26_RCH_MON:
	case AFE_DL26_LCH_MON:
	case AFE_VUL0_CUR_MSB:
	case AFE_VUL0_CUR:
	case AFE_VUL1_CUR_MSB:
	case AFE_VUL1_CUR:
	case AFE_VUL2_CUR_MSB:
	case AFE_VUL2_CUR:
	case AFE_VUL3_CUR_MSB:
	case AFE_VUL3_CUR:
	case AFE_VUL4_CUR_MSB:
	case AFE_VUL4_CUR:
	case AFE_VUL5_CUR_MSB:
	case AFE_VUL5_CUR:
	case AFE_VUL6_CUR_MSB:
	case AFE_VUL6_CUR:
	case AFE_VUL7_CUR_MSB:
	case AFE_VUL7_CUR:
	case AFE_VUL8_CUR_MSB:
	case AFE_VUL8_CUR:
	case AFE_VUL9_CUR_MSB:
	case AFE_VUL9_CUR:
	case AFE_VUL10_CUR_MSB:
	case AFE_VUL10_CUR:
	case AFE_VUL24_CUR_MSB:
	case AFE_VUL24_CUR:
	case AFE_VUL25_CUR_MSB:
	case AFE_VUL25_CUR:
	case AFE_VUL25_RCH_MON:
	case AFE_VUL25_LCH_MON:
	case AFE_VUL26_CUR_MSB:
	case AFE_VUL26_CUR:
	case AFE_VUL_CM0_CUR_MSB:
	case AFE_VUL_CM0_CUR:
	case AFE_VUL_CM1_CUR_MSB:
	case AFE_VUL_CM1_CUR:
	case AFE_VUL_CM2_CUR_MSB:
	case AFE_VUL_CM2_CUR:
	case AFE_ETDM_IN0_CUR_MSB:
	case AFE_ETDM_IN0_CUR:
	case AFE_ETDM_IN1_CUR_MSB:
	case AFE_ETDM_IN1_CUR:
	case AFE_ETDM_IN2_CUR_MSB:
	case AFE_ETDM_IN2_CUR:
	case AFE_ETDM_IN3_CUR_MSB:
	case AFE_ETDM_IN3_CUR:
	case AFE_ETDM_IN4_CUR_MSB:
	case AFE_ETDM_IN4_CUR:
	case AFE_ETDM_IN6_CUR_MSB:
	case AFE_ETDM_IN6_CUR:
	case AFE_HDMI_OUT_CUR_MSB:
	case AFE_HDMI_OUT_CUR:
	case AFE_HDMI_OUT_END:
	case AFE_PROT_SIDEBAND0_MON:
	case AFE_PROT_SIDEBAND1_MON:
	case AFE_PROT_SIDEBAND2_MON:
	case AFE_PROT_SIDEBAND3_MON:
	case AFE_DOMAIN_SIDEBAND0_MON:
	case AFE_DOMAIN_SIDEBAND1_MON:
	case AFE_DOMAIN_SIDEBAND2_MON:
	case AFE_DOMAIN_SIDEBAND3_MON:
	case AFE_DOMAIN_SIDEBAND4_MON:
	case AFE_DOMAIN_SIDEBAND5_MON:
	case AFE_DOMAIN_SIDEBAND6_MON:
	case AFE_DOMAIN_SIDEBAND7_MON:
	case AFE_DOMAIN_SIDEBAND8_MON:
	case AFE_DOMAIN_SIDEBAND9_MON:
	case AFE_PCM0_INTF_CON1_MASK_MON:
	case AFE_PCM0_INTF_CON0_MASK_MON:
	case AFE_CONNSYS_I2S_CON_MASK_MON:
	case AFE_TDM_CON2_MASK_MON:
	case AFE_MTKAIF0_CFG0_MASK_MON:
	case AFE_MTKAIF1_CFG0_MASK_MON:
	case AFE_ADDA_UL0_SRC_CON0_MASK_MON:
	case AFE_ADDA_UL1_SRC_CON0_MASK_MON:
	case AFE_ASRC_NEW_CON0:
	case AFE_ASRC_NEW_CON6:
	case AFE_ASRC_NEW_CON8:
	case AFE_ASRC_NEW_CON9:
	case AFE_ASRC_NEW_CON12:
	case AFE_ASRC_NEW_IP_VERSION:
	case AFE_GASRC0_NEW_CON0:
	case AFE_GASRC0_NEW_CON6:
	case AFE_GASRC0_NEW_CON8:
	case AFE_GASRC0_NEW_CON9:
	case AFE_GASRC0_NEW_CON10:
	case AFE_GASRC0_NEW_CON11:
	case AFE_GASRC0_NEW_CON12:
	case AFE_GASRC0_NEW_IP_VERSION:
	case AFE_GASRC1_NEW_CON0:
	case AFE_GASRC1_NEW_CON6:
	case AFE_GASRC1_NEW_CON8:
	case AFE_GASRC1_NEW_CON9:
	case AFE_GASRC1_NEW_CON12:
	case AFE_GASRC1_NEW_IP_VERSION:
	case AFE_GASRC2_NEW_CON0:
	case AFE_GASRC2_NEW_CON6:
	case AFE_GASRC2_NEW_CON8:
	case AFE_GASRC2_NEW_CON9:
	case AFE_GASRC2_NEW_CON12:
	case AFE_GASRC2_NEW_IP_VERSION:
	case AFE_GASRC3_NEW_CON0:
	case AFE_GASRC3_NEW_CON6:
	case AFE_GASRC3_NEW_CON8:
	case AFE_GASRC3_NEW_CON9:
	case AFE_GASRC3_NEW_CON12:
	case AFE_GASRC3_NEW_IP_VERSION:
	/* these reg would change in adsp */
	case AFE_IRQ_MCU_EN:
	case AFE_IRQ_MCU_DSP_EN:
	case AFE_IRQ_MCU_DSP2_EN:
	case AFE_DL5_CON0:
	case AFE_DL6_CON0:
	case AFE_DL23_CON0:
	case AFE_DL_24CH_CON0:
	case AFE_VUL1_CON0:
	case AFE_VUL3_CON0:
	case AFE_VUL4_CON0:
	case AFE_VUL5_CON0:
	case AFE_VUL9_CON0:
	case AFE_VUL25_CON0:
	case AFE_IRQ0_MCU_CFG0:
	case AFE_IRQ1_MCU_CFG0:
	case AFE_IRQ2_MCU_CFG0:
	case AFE_IRQ3_MCU_CFG0:
	case AFE_IRQ4_MCU_CFG0:
	case AFE_IRQ5_MCU_CFG0:
	case AFE_IRQ6_MCU_CFG0:
	case AFE_IRQ7_MCU_CFG0:
	case AFE_IRQ8_MCU_CFG0:
	case AFE_IRQ9_MCU_CFG0:
	case AFE_IRQ10_MCU_CFG0:
	case AFE_IRQ11_MCU_CFG0:
	case AFE_IRQ12_MCU_CFG0:
	case AFE_IRQ13_MCU_CFG0:
	case AFE_IRQ14_MCU_CFG0:
	case AFE_IRQ15_MCU_CFG0:
	case AFE_IRQ16_MCU_CFG0:
	case AFE_IRQ17_MCU_CFG0:
	case AFE_IRQ18_MCU_CFG0:
	case AFE_IRQ19_MCU_CFG0:
	case AFE_IRQ20_MCU_CFG0:
	case AFE_IRQ21_MCU_CFG0:
	case AFE_IRQ22_MCU_CFG0:
	case AFE_IRQ23_MCU_CFG0:
	case AFE_IRQ24_MCU_CFG0:
	case AFE_IRQ25_MCU_CFG0:
	case AFE_IRQ26_MCU_CFG0:
	case AFE_IRQ0_MCU_CFG1:
	case AFE_IRQ1_MCU_CFG1:
	case AFE_IRQ2_MCU_CFG1:
	case AFE_IRQ3_MCU_CFG1:
	case AFE_IRQ4_MCU_CFG1:
	case AFE_IRQ5_MCU_CFG1:
	case AFE_IRQ6_MCU_CFG1:
	case AFE_IRQ7_MCU_CFG1:
	case AFE_IRQ8_MCU_CFG1:
	case AFE_IRQ9_MCU_CFG1:
	case AFE_IRQ10_MCU_CFG1:
	case AFE_IRQ11_MCU_CFG1:
	case AFE_IRQ12_MCU_CFG1:
	case AFE_IRQ13_MCU_CFG1:
	case AFE_IRQ14_MCU_CFG1:
	case AFE_IRQ15_MCU_CFG1:
	case AFE_IRQ16_MCU_CFG1:
	case AFE_IRQ17_MCU_CFG1:
	case AFE_IRQ18_MCU_CFG1:
	case AFE_IRQ19_MCU_CFG1:
	case AFE_IRQ20_MCU_CFG1:
	case AFE_IRQ21_MCU_CFG1:
	case AFE_IRQ22_MCU_CFG1:
	case AFE_IRQ23_MCU_CFG1:
	case AFE_IRQ24_MCU_CFG1:
	case AFE_IRQ25_MCU_CFG1:
	case AFE_IRQ26_MCU_CFG1:
	/* for vow using */
	case AFE_IRQ_MCU_SCP_EN:
	case AFE_VUL_CM0_BASE_MSB:
	case AFE_VUL_CM0_BASE:
	case AFE_VUL_CM0_END_MSB:
	case AFE_VUL_CM0_END:
	case AFE_VUL_CM0_CON0:
		return true;
	default:
		return false;
	};
}

static const struct regmap_config mt8196_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,

	.volatile_reg = mt8196_is_volatile_reg,

	.max_register = AFE_MAX_REGISTER,
	.num_reg_defaults_raw = AFE_MAX_REGISTER,

	.cache_type = REGCACHE_FLAT,
};

#if defined(FORCE_FPGA_ENABLE_IRQ)
static irqreturn_t mt8196_afe_irq_handler(int irq_id, void *dev)
{
	struct mtk_base_afe *afe = dev;
	struct mtk_base_afe_irq *irq;
	unsigned int status = 0;
	unsigned int status_mcu;
	unsigned int mcu_en = 0;
	unsigned int cus_status = 0;
	unsigned int cus_status_mcu;
	unsigned int cus_mcu_en = 0;
	unsigned int tmp_reg = 0;
	int ret, cus_ret;
	int i;
	struct timespec64 ts64;
	unsigned long long t1, t2;
	/* one interrupt period = 5ms */
	unsigned long long timeout_limit = 5000000;

	/* get irq that is sent to MCU */
	regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &mcu_en);
	regmap_read(afe->regmap, AFE_CUSTOM_IRQ_MCU_EN, &cus_mcu_en);

	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	cus_ret = regmap_read(afe->regmap, AFE_CUSTOM_IRQ_MCU_STATUS, &cus_status);
	/* only care IRQ which is sent to MCU */
	status_mcu = status & mcu_en & AFE_IRQ_STATUS_BITS;
	cus_status_mcu = cus_status & cus_mcu_en & AFE_IRQ_STATUS_BITS;
	if ((ret || (status_mcu == 0)) &&
	    (cus_ret || (cus_status_mcu == 0))) {
		dev_info(afe->dev, "%s(), irq status err, ret %d, status 0x%x, mcu_en 0x%x\n",
			 __func__, ret, status, mcu_en);
		dev_info(afe->dev, "%s(), irq status err, ret %d, cus_status_mcu 0x%x, cus_mcu_en 0x%x\n",
			 __func__, ret, cus_status_mcu, cus_mcu_en);

		goto err_irq;
	}

	ktime_get_ts64(&ts64);
	t1 = timespec64_to_ns(&ts64);

	for (i = 0; i < MT8196_MEMIF_NUM; i++) {
		struct mtk_base_afe_memif *memif = &afe->memif[i];

		if (!memif->substream)
			continue;

		if (memif->irq_usage < 0)
			continue;
		irq = &afe->irqs[memif->irq_usage];

		if (i == MT8196_MEMIF_HDMI) {
			if (cus_status_mcu & (0x1 << irq->irq_data->id))
				snd_pcm_period_elapsed(memif->substream);
		} else {
			if (status_mcu & (0x1 << irq->irq_data->id))
				snd_pcm_period_elapsed(memif->substream);
		}
	}

	ktime_get_ts64(&ts64);
	t2 = timespec64_to_ns(&ts64);
	t2 = t2 - t1; /* in ns (10^9) */

	if (t2 > timeout_limit) {
		dev_info(afe->dev, "%s(), mcu_en 0x%x, cus_mcu_en 0x%x, timeout %llu, limit %llu, ret %d\n",
			__func__, mcu_en, cus_mcu_en,
			t2, timeout_limit, ret);
	}

err_irq:
	/* clear irq */
	for (i = 0; i < MT8196_IRQ_NUM; ++i) {
		if (status_mcu & (0x1 << i)) {
			regmap_read(afe->regmap, irq_data[i].irq_clr_reg, &tmp_reg);
			regmap_update_bits(afe->regmap, irq_data[i].irq_clr_reg,
					AFE_IRQ_CLR_CFG_MASK_SFT | AFE_IRQ_MISS_FLAG_CLR_CFG_MASK_SFT,
					tmp_reg^(AFE_IRQ_CLR_CFG_MASK_SFT | AFE_IRQ_MISS_FLAG_CLR_CFG_MASK_SFT));
		}
	}

	return IRQ_HANDLED;
}
#endif

static int mt8196_afe_runtime_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	unsigned int value = 0;
	unsigned int tmp_reg = 0;
	int ret = 0, i;

	dev_dbg(afe->dev, "%s() ready to stop\n", __func__);

	if (!afe->regmap) {
		dev_info(afe->dev, "%s() skip regmap\n", __func__);
		goto skip_regmap;
	}

	/* Add to be off for free run*/
	/* disable AFE */
	regmap_update_bits(afe->regmap, AUDIO_ENGEN_CON0, 0x1, 0x0);

	ret = regmap_read_poll_timeout(afe->regmap,
				       AUDIO_ENGEN_CON0_MON,
				       value,
				       (value & AUDIO_ENGEN_MON_SFT) == 0,
				       20,
				       1 * 1000 * 1000);
	dev_dbg(afe->dev, "%s() read_poll ret %d\n", __func__, ret);
	if (ret)
		dev_info(afe->dev, "%s(), ret %d\n", __func__, ret);

	/* make sure all irq status are cleared */
	for (i = 0; i < MT8196_IRQ_NUM; ++i) {
		regmap_read(afe->regmap, irq_data[i].irq_clr_reg, &tmp_reg);
		regmap_update_bits(afe->regmap, irq_data[i].irq_clr_reg,
				AFE_IRQ_CLR_CFG_MASK_SFT | AFE_IRQ_MISS_FLAG_CLR_CFG_MASK_SFT,
				tmp_reg^(AFE_IRQ_CLR_CFG_MASK_SFT | AFE_IRQ_MISS_FLAG_CLR_CFG_MASK_SFT));
	}

	/* reset sgen */
	regmap_write(afe->regmap, AFE_SINEGEN_CON0, 0x0);
	regmap_update_bits(afe->regmap, AFE_SINEGEN_CON1,
			   SINE_DOMAIN_MASK_SFT,
			   0x0 << SINE_DOMAIN_SFT);
	regmap_update_bits(afe->regmap, AFE_SINEGEN_CON1,
			   SINE_MODE_MASK_SFT,
			   0x0 << SINE_MODE_SFT);
	regmap_update_bits(afe->regmap, AFE_SINEGEN_CON1,
			   INNER_LOOP_BACKI_SEL_MASK_SFT,
			   0x0 << INNER_LOOP_BACKI_SEL_SFT);
	regmap_update_bits(afe->regmap, AFE_SINEGEN_CON1,
			   INNER_LOOP_BACK_MODE_MASK_SFT,
			   0xff << INNER_LOOP_BACK_MODE_SFT);

	regmap_write(afe->regmap, AUDIO_TOP_CON4, 0x3fff);

	/* reset audio 26M request */
	regmap_update_bits(afe->regmap,
			   AFE_SPM_CONTROL_REQ, 0x1, 0x0);

	/* cache only */
	regcache_cache_only(afe->regmap, true);
	regcache_mark_dirty(afe->regmap);

skip_regmap:
	mt8196_afe_disable_clock(afe);
	return 0;
}

static int mt8196_afe_runtime_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	int ret = 0;

	ret = mt8196_afe_enable_clock(afe);
	dev_dbg(afe->dev, "%s(), enable_clock ret %d\n", __func__, ret);

	if (ret)
		return ret;

	if (!afe->regmap) {
		dev_info(afe->dev, "%s() skip regmap\n", __func__);
		goto skip_regmap;
	}
	regcache_cache_only(afe->regmap, false);
	regcache_sync(afe->regmap);

	/* set audio 26M request */
	regmap_update_bits(afe->regmap, AFE_SPM_CONTROL_REQ, 0x1, 0x1);

	/* IPM2.0: Clear AUDIO_TOP_CON4 for enabling AP side module clk */
	regmap_write(afe->regmap, AUDIO_TOP_CON4, 0x0);

	/* Add to be on for free run */
	regmap_write(afe->regmap, AUDIO_TOP_CON0, 0x0);
	regmap_write(afe->regmap, AUDIO_TOP_CON1, 0x0);
	regmap_write(afe->regmap, AUDIO_TOP_CON2, 0x0);

	/* Can't set AUDIO_TOP_CON3 to be 0x0, it will hang in FPGA env */
	regmap_write(afe->regmap, AUDIO_TOP_CON3, 0x0);

	regmap_update_bits(afe->regmap, AFE_CBIP_CFG0, 0x1, 0x1);

	/* force cpu use 8_24 format when writing 32bit data */
	regmap_update_bits(afe->regmap, AFE_MEMIF_CON0,
			   CPU_HD_ALIGN_MASK_SFT, 0 << CPU_HD_ALIGN_SFT);

	/* enable AFE */
	regmap_update_bits(afe->regmap, AUDIO_ENGEN_CON0, 0x1, 0x1);

skip_regmap:
	return 0;
}

static int mt8196_afe_pcm_copy(struct snd_pcm_substream *substream,
			       int channel, unsigned long hwoff,
			       struct iov_iter *buf, unsigned long bytes,
			       mtk_sp_copy_f sp_copy)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	int ret = 0;

	if (!component)
		return -EINVAL;

	ret = sp_copy(substream, channel, hwoff, buf, bytes);

	return ret;
}

static int mt8196_afe_component_probe(struct snd_soc_component *component)
{
	struct mtk_base_afe *afe = NULL;
	struct snd_soc_card *sndcard = NULL;
	struct snd_card *card = NULL;

	if (component) {
		afe = snd_soc_component_get_drvdata(component);
		sndcard = component->card;
		card = sndcard->snd_card;

		mtk_afe_add_sub_dai_control(component);
	}

	return 0;
}

static const struct snd_soc_component_driver mt8196_afe_component = {
	.name = AFE_PCM_NAME,
	.probe = mt8196_afe_component_probe,
	.pcm_construct = mtk_afe_pcm_new,
	.pcm_destruct = mtk_afe_pcm_free,
	.open = mtk_afe_pcm_open,
	.pointer = mtk_afe_pcm_pointer,
	.copy = mtk_afe_pcm_copy_user,
};

static int mt8196_dai_memif_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mt8196_memif_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mt8196_memif_dai_driver);

	dai->controls = mt8196_pcm_kcontrols;
	dai->num_controls = ARRAY_SIZE(mt8196_pcm_kcontrols);
	dai->dapm_widgets = mt8196_memif_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mt8196_memif_widgets);
	dai->dapm_routes = mt8196_memif_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt8196_memif_routes);
	return 0;
}

typedef int (*dai_register_cb)(struct mtk_base_afe *);
static const dai_register_cb dai_register_cbs[] = {
	mt8196_dai_adda_register,
	mt8196_dai_i2s_register,
	mt8196_dai_tdm_register,
	mt8196_dai_memif_register,
};

static int mt8196_afe_pcm_dev_probe(struct platform_device *pdev)
{
	int ret, i;
	unsigned int tmp_reg = 0;
#if defined(FORCE_FPGA_ENABLE_IRQ)
	int irq_id;
#endif
	struct mtk_base_afe *afe;
	struct mt8196_afe_private *afe_priv;
	struct resource *res;
	struct device *dev;

	pr_info("+%s()\n", __func__);

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret)
		dev_dbg(&pdev->dev, "failed to assign memory region: %d\n", ret);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(34));
	if (ret)
		return ret;

	afe = devm_kzalloc(&pdev->dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	platform_set_drvdata(pdev, afe);

	afe->platform_priv = devm_kzalloc(&pdev->dev, sizeof(*afe_priv),
					  GFP_KERNEL);
	if (!afe->platform_priv)
		return -ENOMEM;

	afe_priv = afe->platform_priv;

	afe->dev = &pdev->dev;
	dev = afe->dev;

	/* init audio related clock */
	ret = mt8196_init_clock(afe);
	if (ret) {
		dev_info(dev, "init clock error: %d\n", ret);
		return ret;
	}

	pm_runtime_enable(&pdev->dev);
	if (!pm_runtime_enabled(&pdev->dev))
		goto err_pm_disable;

	/* Audio device is part of genpd.
	 * Set audio as syscore device to prevent
	 * genpd automatically power off audio
	 * device when suspend
	 */
	dev_pm_syscore_device(&pdev->dev, true);

	/* regmap init */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	afe->base_addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(afe->base_addr))
		return PTR_ERR(afe->base_addr);

	/* enable clock for regcache get default value from hw */
	pm_runtime_get_sync(&pdev->dev);

	afe->regmap = devm_regmap_init_mmio(&pdev->dev, afe->base_addr,
					    &mt8196_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	/* IPM2.0 clock flow, need debug */

	regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &tmp_reg);
	regmap_write(afe->regmap, AFE_IRQ_MCU_EN, 0xffffffff);
	regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &tmp_reg);
	/* IPM2.0 clock flow, need debug */

	pm_runtime_put_sync(&pdev->dev);

	regcache_cache_only(afe->regmap, true);
	regcache_mark_dirty(afe->regmap);

	/* init gpio */
	ret = mt8196_afe_gpio_init(afe);
	if (ret)
		dev_info(dev, "init gpio error\n");

	/* init memif */
	/* IPM2.0 no need banding */
	afe->is_memif_bit_banding = 0;
	afe->memif_32bit_supported = 1;
	afe->memif_size = MT8196_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);

	if (!afe->memif)
		return -ENOMEM;

	for (i = 0; i < afe->memif_size; i++) {
		afe->memif[i].data = &memif_data[i];
		afe->memif[i].irq_usage = memif_irq_usage[i];
		afe->memif[i].const_irq = 1;
	}

	mutex_init(&afe->irq_alloc_lock);       /* needed when dynamic irq */

	/* init irq */
	afe->irqs_size = MT8196_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);

	if (!afe->irqs)
		return -ENOMEM;

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &irq_data[i];

#if defined(FORCE_FPGA_ENABLE_IRQ)
	/* request irq */
	irq_id = platform_get_irq(pdev, 0);
	if (irq_id <= 0) {
		dev_info(dev, "%pOFn no irq found\n", dev->of_node);
		return irq_id < 0 ? irq_id : -ENXIO;
	}
	ret = devm_request_irq(dev, irq_id, mt8196_afe_irq_handler,
			       IRQF_TRIGGER_NONE,
			       "Afe_ISR_Handle", (void *)afe);
	if (ret) {
		dev_info(dev, "could not request_irq for Afe_ISR_Handle\n");
		return ret;
	}
	ret = enable_irq_wake(irq_id);
	if (ret < 0)
		dev_info(dev, "enable_irq_wake %d err: %d\n", irq_id, ret);
#endif

	/* init sub_dais */
	INIT_LIST_HEAD(&afe->sub_dais);

	for (i = 0; i < ARRAY_SIZE(dai_register_cbs); i++) {
		ret = dai_register_cbs[i](afe);
		if (ret) {
			dev_info(afe->dev, "dai register i %d fail, ret %d\n",
				 i, ret);
			goto err_pm_disable;
		}
	}

	/* init dai_driver and component_driver */
	ret = mtk_afe_combine_sub_dai(afe);
	if (ret) {
		dev_info(afe->dev, "mtk_afe_combine_sub_dai fail, ret %d\n",
			 ret);
		goto err_pm_disable;
	}

	/* others */
	afe->mtk_afe_hardware = &mt8196_afe_hardware;
	afe->memif_fs = mt8196_memif_fs;
	afe->irq_fs = mt8196_irq_fs;
	afe->get_dai_fs = mt8196_get_dai_fs;
	afe->get_memif_pbuf_size = mt8196_get_memif_pbuf_size;

	afe->runtime_resume = mt8196_afe_runtime_resume;
	afe->runtime_suspend = mt8196_afe_runtime_suspend;

	afe->request_dram_resource = mt8196_afe_dram_request;
	afe->release_dram_resource = mt8196_afe_dram_release;

	afe->copy = mt8196_afe_pcm_copy;

	/* register component */
	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt8196_afe_component,
					      afe->dai_drivers,
					      afe->num_dai_drivers);
	if (ret) {
		dev_info(dev, "afe component err: %d\n", ret);
		goto err_pm_disable;
	}
	return 0;

err_pm_disable:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int mt8196_afe_pcm_dev_remove(struct platform_device *pdev)
{
	struct mtk_base_afe *afe = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		mt8196_afe_runtime_suspend(&pdev->dev);

	/* disable afe clock */
	mt8196_afe_disable_clock(afe);
	return 0;
}

static const struct of_device_id mt8196_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt8196-sound", },
	{},
};
MODULE_DEVICE_TABLE(of, mt8196_afe_pcm_dt_match);

static const struct dev_pm_ops mt8196_afe_pm_ops = {
	SET_RUNTIME_PM_OPS(mt8196_afe_runtime_suspend,
			   mt8196_afe_runtime_resume, NULL)
};

static struct platform_driver mt8196_afe_pcm_driver = {
	.driver = {
		.name = "mt8196-audio",
		.of_match_table = mt8196_afe_pcm_dt_match,
#if IS_ENABLED(CONFIG_PM)
		.pm = &mt8196_afe_pm_ops,
#endif
	},
	.probe = mt8196_afe_pcm_dev_probe,
	.remove = mt8196_afe_pcm_dev_remove,
};

module_platform_driver(mt8196_afe_pcm_driver);

MODULE_DESCRIPTION("Mediatek ALSA SoC AFE platform driver for 8196");
MODULE_AUTHOR("Darren Ye <darren.ye@mediatek.com>");
MODULE_LICENSE("GPL");
