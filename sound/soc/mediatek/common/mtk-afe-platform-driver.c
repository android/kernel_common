// SPDX-License-Identifier: GPL-2.0
/*
 * mtk-afe-platform-driver.c  --  Mediatek afe platform driver
 *
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Garlic Tseng <garlic.tseng@mediatek.com>
 */

#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <sound/soc.h>

#include "mtk-afe-platform-driver.h"
#include "mtk-base-afe.h"

int mtk_afe_combine_sub_dai(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;
	size_t num_dai_drivers = 0, dai_idx = 0;

	/* calcualte total dai driver size */
	list_for_each_entry(dai, &afe->sub_dais, list) {
		num_dai_drivers += dai->num_dai_drivers;
	}

	dev_info(afe->dev, "%s(), num of dai %zd\n", __func__, num_dai_drivers);

	/* combine sub_dais */
	afe->num_dai_drivers = num_dai_drivers;
	afe->dai_drivers = devm_kcalloc(afe->dev,
					num_dai_drivers,
					sizeof(struct snd_soc_dai_driver),
					GFP_KERNEL);
	if (!afe->dai_drivers)
		return -ENOMEM;

	list_for_each_entry(dai, &afe->sub_dais, list) {
		/* dai driver */
		memcpy(&afe->dai_drivers[dai_idx],
		       dai->dai_drivers,
		       dai->num_dai_drivers *
		       sizeof(struct snd_soc_dai_driver));
		dai_idx += dai->num_dai_drivers;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_afe_combine_sub_dai);

int mtk_afe_add_sub_dai_control(struct snd_soc_component *component)
{
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	struct mtk_base_afe_dai *dai;

	list_for_each_entry(dai, &afe->sub_dais, list) {
		if (dai->controls)
			snd_soc_add_component_controls(component,
						       dai->controls,
						       dai->num_controls);

		if (dai->dapm_widgets)
			snd_soc_dapm_new_controls(&component->dapm,
						  dai->dapm_widgets,
						  dai->num_dapm_widgets);
	}
	/* add routes after all widgets are added */
	list_for_each_entry(dai, &afe->sub_dais, list) {
		if (dai->dapm_routes)
			snd_soc_dapm_add_routes(&component->dapm,
						dai->dapm_routes,
						dai->num_dapm_routes);
	}

	snd_soc_dapm_new_widgets(component->dapm.card);

	return 0;

}
EXPORT_SYMBOL_GPL(mtk_afe_add_sub_dai_control);

int mtk_afe_pcm_open(struct snd_soc_component *component,
		     struct snd_pcm_substream *substream)
{
	/* set the wait_for_avail to 2 sec*/
	substream->wait_time = msecs_to_jiffies(2 * 1000);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_afe_pcm_open);

snd_pcm_uframes_t mtk_afe_pcm_pointer(struct snd_soc_component *component,
				      struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	struct mtk_base_afe_memif *memif = &afe->memif[snd_soc_rtd_to_cpu(rtd, 0)->id];
	const struct mtk_base_memif_data *memif_data = memif->data;
	struct regmap *regmap = afe->regmap;
	struct device *dev = afe->dev;
	unsigned int hw_ptr_lower32 = 0, hw_ptr_upper32 = 0;
	unsigned int hw_base_lower32 = 0, hw_base_upper32 = 0;
	unsigned long long hw_ptr = 0, hw_base = 0;
	int ret;
	unsigned long long pcm_ptr_bytes = 0;

	ret = regmap_read(regmap, memif_data->reg_ofs_cur, &hw_ptr_lower32);
	if (ret || hw_ptr_lower32 == 0) {
		dev_err(dev, "%s hw_ptr_lower32 err\n", __func__);
		goto POINTER_RETURN_FRAMES;
	}

	if (memif_data->reg_ofs_cur_msb) {
		ret = regmap_read(regmap, memif_data->reg_ofs_cur_msb, &hw_ptr_upper32);
		if (ret) {
			dev_err(dev, "%s hw_ptr_upper32 err\n", __func__);
			goto POINTER_RETURN_FRAMES;
		}
	}

	ret = regmap_read(regmap, memif_data->reg_ofs_base, &hw_base_lower32);
	if (ret || hw_base_lower32 == 0) {
		dev_err(dev, "%s hw_base_lower32 err\n", __func__);
		goto POINTER_RETURN_FRAMES;
	}
	if (memif_data->reg_ofs_base_msb) {
		ret = regmap_read(regmap, memif_data->reg_ofs_base_msb, &hw_base_upper32);
		if (ret) {
			dev_err(dev, "%s hw_base_upper32 err\n", __func__);
			goto POINTER_RETURN_FRAMES;
		}
	}
	hw_ptr = ((unsigned long long)hw_ptr_upper32 << 32) + hw_ptr_lower32;
	hw_base = ((unsigned long long)hw_base_upper32 << 32) + hw_base_lower32;

POINTER_RETURN_FRAMES:
	pcm_ptr_bytes = MTK_WORD_SIZE_ALIGN(hw_ptr - hw_base);
	return bytes_to_frames(substream->runtime, (ssize_t)pcm_ptr_bytes);
}
EXPORT_SYMBOL_GPL(mtk_afe_pcm_pointer);

/* calculate the target DMA-buffer position to be written/read */
static void *get_dma_ptr(struct snd_pcm_runtime *runtime,
			   int channel, unsigned long hwoff)
{
	return runtime->dma_area + hwoff +
		channel * (runtime->dma_bytes / runtime->channels);
}

/* default copy_user ops for write; used for both interleaved and non- modes */
static int default_write_copy(struct snd_pcm_substream *substream,
			      int channel, unsigned long hwoff,
			      struct iov_iter *iter, unsigned long bytes)
{
	if (copy_from_iter(get_dma_ptr(substream->runtime, channel, hwoff),
			   bytes, iter) != bytes)
		return -EFAULT;
	return 0;
}

/* default copy_user ops for read; used for both interleaved and non- modes */
static int default_read_copy(struct snd_pcm_substream *substream,
			     int channel, unsigned long hwoff,
			     struct iov_iter *iter, unsigned long bytes)
{
	if (copy_to_iter(get_dma_ptr(substream->runtime, channel, hwoff),
			 bytes, iter) != bytes)
		return -EFAULT;
	return 0;
}

int mtk_afe_pcm_copy_user(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream,
			  int channel, unsigned long hwoff,
			  struct iov_iter *buf, unsigned long bytes)
{
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	int is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	mtk_sp_copy_f sp_copy;
	int ret;

	sp_copy = is_playback ? default_write_copy : default_read_copy;

	if (afe->copy) {
		ret = afe->copy(substream, channel, hwoff,
				buf, bytes, sp_copy);
		if (ret)
			return -EFAULT;
	} else {
		sp_copy(substream, channel, hwoff,
			buf, bytes);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_afe_pcm_copy_user);

int mtk_afe_pcm_ack(struct snd_pcm_substream *substream)
{
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_afe_pcm_ack);

int mtk_afe_pcm_new(struct snd_soc_component *component,
		    struct snd_soc_pcm_runtime *rtd)
{
	size_t size;
	struct snd_pcm *pcm = rtd->pcm;
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	size = afe->mtk_afe_hardware->buffer_bytes_max;
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, afe->dev, 0, size);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_afe_pcm_new);

static int mtk_afe_component_probe(struct snd_soc_component *component)
{
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	int ret;

	snd_soc_component_init_regmap(component, afe->regmap);

	/* If the list was never initialized there are no sub-DAIs */
	if (afe->sub_dais.next && afe->sub_dais.prev) {
		ret = mtk_afe_add_sub_dai_control(component);
		if (ret)
			return ret;
	}

	return 0;
}
void mtk_afe_pcm_free(struct snd_soc_component *component, struct snd_pcm *pcm)
{
	snd_pcm_lib_preallocate_free_for_all(pcm);
}
EXPORT_SYMBOL_GPL(mtk_afe_pcm_free);

const struct snd_soc_component_driver mtk_afe_pcm_platform = {
	.name		= AFE_PCM_NAME,
	.pointer	= mtk_afe_pcm_pointer,
	.pcm_construct	= mtk_afe_pcm_new,
	.probe		= mtk_afe_component_probe,
	.pcm_destruct	= mtk_afe_pcm_free,
};
EXPORT_SYMBOL_GPL(mtk_afe_pcm_platform);

MODULE_DESCRIPTION("Mediatek simple platform driver");
MODULE_AUTHOR("Garlic Tseng <garlic.tseng@mediatek.com>");
MODULE_LICENSE("GPL v2");

