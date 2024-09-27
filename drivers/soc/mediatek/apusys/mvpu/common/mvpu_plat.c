// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include "mvpu_plat.h"

struct mvpu_platdata *g_mvpu_platdata;

int mvpu_drv_loglv;

static const struct of_device_id mvpu_of_match[] = {
	{
	.compatible = "mediatek, mt8196-mvpu",
	.data = &mvpu_mt8196_platdata
	},
	{
	/* end of list */
	},
};

const struct of_device_id *mvpu_plat_get_device(void)
{
	return mvpu_of_match;
}

int mvpu_platdata_init(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mvpu_platdata *platdata;
	unsigned int dts_ver;

	if (g_mvpu_platdata != NULL) {
		dev_info(dev, "%s: already init, by pass\n", __func__);
		return 0;
	}

	platdata = (struct mvpu_platdata *)of_device_get_match_data(dev);

	if (!platdata) {
		dev_info(dev, "%s: get of_device_get_match_data fail\n", __func__);
		return -EINVAL;
	}

	dev_info(dev, "%s: platdata_sw_ver = %d\n", __func__, platdata->sw_ver);
	dev_info(dev, "%s: sw_preemption_level = %d\n",
			__func__, platdata->sw_preemption_level);

	if (of_property_read_u32(dev->of_node, "version", &dts_ver) == 0)
		dev_info(dev, "%s: dts_ver = %d\n", __func__, dts_ver);

	if (!of_property_read_u32(dev->of_node, "core_num", &platdata->core_num)) {
	} else if (!of_property_read_u32(dev->of_node, "core-num", &platdata->core_num)) {
	} else {
		dev_info(dev, "%s: get core num fail\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "%s: core-num = %d\n", __func__, platdata->core_num);

	if (platdata->core_num > MAX_CORE_NUM) {
		dev_info(dev, "%s: invalid core number: %d\n", __func__, platdata->core_num);
		return -EINVAL;
	}

	if (of_property_read_u64(dev->of_node, "mask", &platdata->dma_mask)) {
		dev_info(dev, "%s: get mask fail\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "%s: mask = 0x%llx\n", __func__, platdata->dma_mask);

	platdata->pdev = pdev;
	g_mvpu_platdata = platdata;

	return 0;
}
