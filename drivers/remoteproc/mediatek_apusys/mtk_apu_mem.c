// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/dma-mapping.h>

#include "linux/remoteproc/mtk_apu.h"

int mtk_apu_mem_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret;

	if (apu->platdata->flags.bypass_iommu)
		return 0;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "%s: dma_set_mask_and_coherent failed: %d\n", __func__, ret);
		return ret;
	}

	return 0;
}
