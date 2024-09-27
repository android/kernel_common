// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "linux/soc/mediatek/mtk_apu_device.h"
#include "linux/remoteproc/mtk_apu_config.h"

#include "mvpu_plat.h"
#include "mvpu_sysfs.h"
#include "mvpu_driver.h"

static struct apusys_device mvpu_apusys_dev;

static int mvpu_dma_init(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	uint64_t mask = g_mvpu_platdata->dma_mask;

	ret = dma_set_mask_and_coherent(dev, mask);
	if (ret) {
		dev_info(dev, "%s: unable to set DMA mask coherent: %d\n", __func__, ret);
		return ret;
	}

	dev_info(dev, "%s: dma_set_mask_and_coherent 0x%llx\n", __func__, mask);

	ret = dma_set_mask(dev, mask);
	if (ret) {
		dev_info(dev, "%s: unable to set DMA mask: %d\n", __func__, ret);
		return ret;
	}

	dev_info(dev, "%s: dma_set_mask 0x%llx\n", __func__, mask);

	return ret;
}

static int mvpu_apusys_register(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;

	mvpu_apusys_dev.dev_type = APUSYS_DEVICE_MVPU;
	mvpu_apusys_dev.preempt_type = APUSYS_PREEMPT_NONE;
	mvpu_apusys_dev.preempt_level = 0;
	mvpu_apusys_dev.send_cmd = g_mvpu_platdata->ops->mvpu_handler_lite;
	mvpu_apusys_dev.idx = 0;

	ret = apusys_register_device(&mvpu_apusys_dev);
	if (ret)
		dev_info(dev, "%s: failed to register apusys (%d)\n", __func__, ret);

	return ret;
}

static int mvpu_suspend(struct platform_device *pdev, pm_message_t mesg)
{
	struct device *dev = &pdev->dev;

	dev_info(dev, "%s +\n", __func__);
	return 0;
}

static int mvpu_resume(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	dev_info(dev, "%s +\n", __func__);
	return 0;
}

static int mvpu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	dev_info(dev, "%s +\n", __func__);

	if (mvpu_platdata_init(pdev)) {
		dev_info(dev, "%s: get plat info fail\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "%s: get plat info pass\n", __func__);

	if (mvpu_apusys_register(pdev)) {
		dev_info(dev, "%s: apusys dev register fail, defer probe\n", __func__);
		return -EPROBE_DEFER;
	}
	dev_info(dev, "%s: apusys dev register pass\n", __func__);

	g_mvpu_platdata->ops->mvpu_handler_lite_init();

	if (mvpu_dma_init(pdev)) {
		dev_info(dev, "%s: dma init fail\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "%s: dma init pass\n", __func__);

	if (g_mvpu_platdata->sw_ver == MVPU_SW_VER_MVPU20 || g_mvpu_platdata->sw_ver == MVPU_SW_VER_MVPU25 ||
		g_mvpu_platdata->sw_ver == MVPU_SW_VER_MVPU25a) {
		if (g_mvpu_platdata->sec_ops->mvpu_sec_init(dev)) {
			dev_info(dev, "%s: sec init fail\n", __func__);
			return -EINVAL;
		}
		dev_info(dev, "%s: sec init pass\n", __func__);

		if (g_mvpu_platdata->sec_ops->mvpu_load_img(dev)) {
			dev_info(dev, "%s: load img fail\n", __func__);
			return -EINVAL;
		}
		dev_info(dev, "%s: load img pass\n", __func__);
	}

	if (mvpu_sysfs_init()) {
		dev_info(dev, "%s: sysfs init fail\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "%s: sysfs init pass\n", __func__);

	if (g_mvpu_platdata->ops->mvpu_ipi_init()) {
		dev_info(dev, "%s: ipi init fail\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "%s: ipi init pass\n", __func__);

	dev_info(dev, "%s probe pass\n", __func__);
	return 0;
}

static int mvpu_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	dev_info(dev, "%s +\n", __func__);
	g_mvpu_platdata->ops->mvpu_ipi_deinit();
	mvpu_sysfs_exit();

	return 0;
}

static struct platform_driver mvpu_driver = {
	.probe = mvpu_probe,
	.remove = mvpu_remove,
	.suspend = mvpu_suspend,
	.resume = mvpu_resume,
	.driver = {
		.name = "mtk_mvpu",
		.owner = THIS_MODULE,
	}
};

int mvpu_init(void)
{
	pr_info("%s +\n", __func__);
	mvpu_driver.driver.of_match_table = mvpu_plat_get_device();

	if (platform_driver_register(&mvpu_driver) != 0) {
		pr_info("%s: register platform driver fail\n", __func__);
		return -ENODEV;
	}
	pr_info("%s: register platform driver pass\n", __func__);

	return 0;
}

void mvpu_exit(void)
{
	platform_driver_unregister(&mvpu_driver);
}

MODULE_IMPORT_NS(DMA_BUF);
MODULE_IMPORT_NS(MTK_APU_MDW);

module_init(mvpu_init);
module_exit(mvpu_exit);
MODULE_DESCRIPTION("APU MVPU Driver");
MODULE_LICENSE("GPL");
