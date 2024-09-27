// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/module.h>  /* Needed by all modules */
#include <linux/kernel.h>  /* Needed for KERN_ALERT */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>

#include <linux/init.h>
#include <linux/io.h>
#include <linux/rpmsg.h>

#include "../platform/apummu_plat.h"
#include "../platform/apummu_device.h"

#if IS_ENABLED(CONFIG_OF)
#include <linux/of.h>
#include <linux/of_platform.h>
#endif
#include "linux/remoteproc/mtk_apu.h"
#include "linux/remoteproc/mtk_apu_config.h"

#include "slbc_ops.h"

/* import apummu header */
#include "apummu_drv.h"
#include "apummu_cmn.h"
#include "apummu_dbg.h"
#include "apummu_mem.h"
#include "apummu_remote.h"
#include "apummu_remote_cmd.h"
#include "apummu_mgt.h"
#include "linux/soc/mediatek/apummu_export.h"

/* define */
#define APUSYS_DRV_NAME "apusys_drv_apummu"
#define APUSYS_DEV_NAME "apusys_apummu"

#define SLB_NODE	(0)

MODULE_IMPORT_NS(DMA_BUF);

/* global variable */
static struct class *apummu_class;
struct apummu_dev_info *g_adv;
static struct task_struct *mem_task;
static struct dentry *apummu_dbg_root;
static int apummu_suspend_release_flag;

/* function declaration */
static int apummu_open(struct inode *, struct file *);
static int apummu_release(struct inode *, struct file *);
#if !(DRAM_FALL_BACK_IN_RUNTIME)
static int apummu_memory_func(void *arg);
#endif
static int apummu_map_dts(struct platform_device *pdev);
static int apummu_create_node(struct platform_device *pdev);
static int apummu_delete_node(void *drvinfo);

struct apu_ipi_apummu_rx_rpmsg_device {
	struct rpmsg_endpoint *ept;
	struct rpmsg_device *rpdev;
};
static struct apu_ipi_apummu_rx_rpmsg_device apu_ipi_apummmu_rx_rpm_dev;
static const struct of_device_id apu_apummu_rx_rpmsg_of_match[] = {
	{ .compatible = "mediatek,apu-apummu-rx", },
	{ },
};
static int apu_ipi_apummu_rx_rpmsg_probe(struct rpmsg_device *rpdev)
{
	AMMU_LOG_INFO("%s: name=%s, src=%d\n",
			__func__, rpdev->id.name, rpdev->src);

	apu_ipi_apummmu_rx_rpm_dev.ept = rpdev->ept;
	apu_ipi_apummmu_rx_rpm_dev.rpdev = rpdev;

	AMMU_LOG_INFO("%s: rpdev->ept = %p\n", __func__, rpdev->ept);

	return 0;
}
static int apu_ipi_apummu_rx_rpmsg_cb(struct rpmsg_device *rpdev, void *data,
		int len, void *priv, u32 src)
{
	struct apummu_rx_data *d = (struct apummu_rx_data *)data;

	switch (d->module_id) {
	case APUMMU_RX_TEST:
		pr_info("%s: received APUMMU_RX_TEST from uP\n", __func__);
		break;
	case APUMMU_RX_APUMMU_AEE:
		AMMU_LOG_ERR("APUMMU error in uP, error_code = %d\n", d->error_code);
		apusys_ammu_exception("APUMMU error in uP");
		break;
	case APUMMU_RX_HSE_AEE:
		pr_info("HSE error in uP, error_code = %d\n", d->error_code);
		apusys_hse_exception("HSE error in uP");
		break;
	case APUMMU_RX_CBFC_AEE:
		pr_info("CBFC error in uP, error_code = %d\n", d->error_code);
		apusys_cbfc_exception("CBFC error in uP");
		break;
	default:
		pr_info("%s: unknown cmd %d\n", __func__, d->module_id);
		break;
	}

	rpmsg_send(apu_ipi_apummmu_rx_rpm_dev.ept, data, sizeof(struct apummu_rx_data));

	return 0;
}

static void apummu_ipi_apummu_rx_rpmsg_remove(struct rpmsg_device *rpdev)
{
	AMMU_LOG_INFO("apummu_ipi_apummu_rx remove Done\n");
}
static struct rpmsg_driver apu_ipi_apummu_rx_rpmsg_driver = {
	.drv = {
		.name = "apu-apummu-rx",
		.owner = THIS_MODULE,
		.of_match_table = apu_apummu_rx_rpmsg_of_match,
	},
	.probe    = apu_ipi_apummu_rx_rpmsg_probe,
	.remove   = apummu_ipi_apummu_rx_rpmsg_remove,
	.callback = apu_ipi_apummu_rx_rpmsg_cb,
};

#if !(DRAM_FALL_BACK_IN_RUNTIME)
/* alloc DRAM for fallback */
static int apummu_memory_func(void *arg)
{
	int ret = 0;
	struct apummu_dev_info *adv;

	adv = (struct apummu_dev_info *) arg;

	ret = apummu_dram_remap_alloc(adv);
	if (ret) {
		AMMU_LOG_ERR("Could not set memory for apummu\n");
		ret = -ENOMEM;
		goto out;
	}
	AMMU_LOG_INFO("apummu memory init\n");

out:
	return ret;
}
#endif

/* RV HS and set RV DRAM fallback */
static int apummu_rprmsg_memory_func(void *arg)
{
	struct apummu_dev_info *adv;
	int ret = 0;
#if !(DRAM_FALL_BACK_IN_RUNTIME)
	uint32_t i;
#endif

	adv = (struct apummu_dev_info *) arg;

	ret = apummu_remote_handshake(adv, NULL);
	if (ret) {
		AMMU_LOG_ERR("Remote Handshake fail %d\n", ret);
		goto out;
	}

#if !(DRAM_FALL_BACK_IN_RUNTIME)
	ret = apummu_memory_func(arg);
	if (ret) {
		AMMU_LOG_ERR("apummu memory fail\n");
		goto out;
	}

	for (i = 0; i < adv->remote.dram_max; i++) {
		ret = apummu_remote_set_hw_default_iova(adv, i, adv->remote.dram[i]);
		if (ret) {
			AMMU_LOG_ERR("apummu_remote_set_hw_default_iova fail %d\n", ret);
			goto out;
		}
	}
#endif

	AMMU_LOG_INFO("apummu rprmsg remote init done\n");

out:
	return ret;
}


int apummu_set_init_info(struct mtk_apu *apu)
{
	struct apummu_dev_info *adv;
	struct mtk_apummu_init_info *rv_info;
	// int i = 0;

	adv = g_adv;

	if (!adv) {
		AMMU_LOG_ERR("No apummu_dev_info!\n");
		return -EINVAL;
	}

	rv_info = (struct mtk_apummu_init_info *)
			get_mtk_apu_config_user_ptr(apu->conf_buf, eREVISER_INIT_INFO);

	memset((void *)rv_info, 0, sizeof(struct mtk_apummu_init_info));

	/* NOTE: since alloc in runtime, this info may not be right */
	rv_info->boundary = adv->plat.boundary;
	// for (i = 0; i < adv->remote.dram_max; i++)
	// rv_info->dram[i] = adv->remote.dram[i];

	AMMU_LOG_INFO("apummu info init\n");

	return 0;
}
EXPORT_SYMBOL(apummu_set_init_info);

static const struct file_operations apummu_fops = {
	.open = apummu_open,
	.release = apummu_release,
};


static int apummu_open(struct inode *inode, struct file *filp)
{
	struct apummu_dev_info *adv;

	adv = container_of(inode->i_cdev,
			struct apummu_dev_info, apummu_cdev);

	filp->private_data = adv;
	AMMU_LOG_INFO("adv  %p\n", adv);
	AMMU_LOG_INFO("filp->private_data  %p\n", filp->private_data);
	return 0;
}

static int apummu_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static int apummu_map_dts(struct platform_device *pdev)
{
	int ret = 0;
	struct apummu_dev_info *adv = platform_get_drvdata(pdev);

#if SLB_NODE
	uint32_t slb_size = 0;
	struct device_node *slb_node;
#endif

	if (!adv) {
		AMMU_LOG_ERR("No apummu_dev_info!\n");
		ret = -EINVAL;
		goto out;
	}

	/* boundary can also take from HS */
	adv->plat.boundary = 0;

#if SLB_NODE
	slb_node = of_find_compatible_node(
			NULL, NULL, "mediatek,mtk-slbc");
	if (slb_node) {
		of_property_read_u32(slb_node, "apu", &slb_size);
		adv->rsc.pool[APUMMU_POOL_SLBS].size = slb_size;
		AMMU_LOG_INFO("APU-slb size: 0x%x\n", adv->rsc.pool[APUMMU_POOL_SLBS].size);
	}
#endif

out:
	return ret;
}

static int apummu_create_node(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct apummu_dev_info *adv = platform_get_drvdata(pdev);

	if (!adv) {
		AMMU_LOG_ERR("No apummu_dev_info!\n");
		return -EINVAL;
	}

	/* get major */
	ret = alloc_chrdev_region(&adv->apummu_devt,
			0, 1, APUSYS_DRV_NAME);
	if (ret < 0) {
		AMMU_LOG_ERR("alloc_chrdev_region failed, %d\n", ret);
		goto out;
	}

	/* Attach file operation. */
	cdev_init(&adv->apummu_cdev, &apummu_fops);
	adv->apummu_cdev.owner = THIS_MODULE;

	/* Add to system */
	ret = cdev_add(&adv->apummu_cdev,
			adv->apummu_devt, 1);
	if (ret < 0) {
		AMMU_LOG_ERR("Attach file operation failed, %d\n", ret);
		goto free_chrdev_region;
	}

	/* Create class register */
	apummu_class = class_create(APUSYS_DRV_NAME);
	if (IS_ERR(apummu_class)) {
		ret = PTR_ERR(apummu_class);
		AMMU_LOG_ERR("Unable to create class, err = %d\n", ret);
		goto free_cdev_add;
	}

	dev = device_create(apummu_class, NULL, adv->apummu_devt,
				NULL, APUSYS_DEV_NAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		AMMU_LOG_ERR("Failed to create device: /dev/%s, err = %d",
			APUSYS_DEV_NAME, ret);
		goto free_class;
	}

	return 0;

free_class:
	/* Release class */
	class_destroy(apummu_class);
free_cdev_add:
	/* Release char driver */
	cdev_del(&adv->apummu_cdev);

free_chrdev_region:
	unregister_chrdev_region(adv->apummu_devt, 1);
out:
	return ret;
}

static int apummu_delete_node(void *drvinfo)
{
	int ret = 0;
	struct apummu_dev_info *adv = NULL;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	device_destroy(apummu_class, adv->apummu_devt);
	class_destroy(apummu_class);
	cdev_del(&adv->apummu_cdev);
	unregister_chrdev_region(adv->apummu_devt, 1);

	return ret;
}

static int apummu_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct apummu_dev_info *adv;

	g_adv = NULL;
	mem_task = NULL;

	adv = devm_kzalloc(dev, sizeof(*adv), GFP_KERNEL);
	if (!adv) {
		ret = -ENOMEM;
		goto out;
	}

	adv->dev = &pdev->dev;

	platform_set_drvdata(pdev, adv);
	dev_set_drvdata(dev, adv);

	// add for VLM DRAM 4-16G
	ret = dma_set_mask_and_coherent(adv->dev, DMA_BIT_MASK(34));
	if (ret) {
		AMMU_LOG_ERR("dma_set_mask_and_coherent fail\n");
		goto out;
	}

	adv->init_done = false;

	if (apummu_plat_init(pdev)) {
		dev_info(dev, "platform init failed\n");
		ret = -ENODEV;
		goto out;
	}

	if (apummu_create_node(pdev)) {
		AMMU_LOG_ERR("apummu_create_node fail\n");
		ret = -ENODEV;
		goto out;
	}

	if (apummu_map_dts(pdev)) {
		AMMU_LOG_ERR("apummu_map_dts fail\n");
		ret = -ENODEV;
		goto free_node;
	}

	apummu_dbg_init(adv, apummu_dbg_root);

	apummu_mgt_init();
	apummu_mem_init();
	if (apummu_export_API_init()) {
		AMMU_LOG_ERR("export API init fail\n");
		ret = -EINVAL;
		goto free_node;
	}
	g_adv = adv;

	adv->init_done = true;
	AMMU_LOG_INFO("apummu probe done\n");

out:
	return ret;
free_node:
	apummu_delete_node(adv);
	return ret;
}

static int apummu_remove(struct platform_device *pdev)
{
	struct apummu_dev_info *adv = platform_get_drvdata(pdev);

	apummu_dbg_destroy(adv);
	apummu_mgt_destroy();
	if (mem_task) {
	#if !(DRAM_FALL_BACK_IN_RUNTIME)
		apummu_dram_remap_free(adv);
	#endif
		mem_task = NULL;
	}
	apummu_delete_node(adv);

	g_adv = NULL;

	AMMU_LOG_INFO("remove done\n");
	return 0;
}

static int apusys_apummu_resume(struct device *dev)
{
	int ret = 0;
	// SLB data init
	struct slbc_data slb_data = {
		.uid = UID_APU,
		.type = TP_BUFFER,
	};

	mutex_lock(&g_ammu_table_set.table_lock);
	if (g_adv->plat.is_general_SLB_support)
		if (!g_ammu_table_set.is_SLB_alloc
			&& apummu_suspend_release_flag) {
			ret = slbc_request(&slb_data);
			if (ret) {
				AMMU_LOG_ERR("general SLB alloc fail in resume...\n");
				apusys_ammu_exception("Alloc SLB fail in resume\n");
				goto out;
			}

			g_ammu_table_set.is_SLB_alloc = true;
			apummu_suspend_release_flag = false;
		}

out:
	mutex_unlock(&g_ammu_table_set.table_lock);
	return ret;
}

static int apusys_apummu_suspend(struct device *dev)
{
	int ret = 0;
	// SLB data init
	struct slbc_data slb_data = {
		.uid = UID_APU,
		.type = TP_BUFFER,
	};

	mutex_lock(&g_ammu_table_set.table_lock);
	if (g_adv->plat.is_general_SLB_support)
		if (g_ammu_table_set.is_SLB_alloc) {
			ret = slbc_release(&slb_data);
			if (ret) {
				AMMU_LOG_ERR("general SLB free fail in suspend...\n");
				apusys_ammu_exception("Free SLB fail in suspend\n");
				goto out;
			}

			apummu_suspend_release_flag = true;
			g_ammu_table_set.is_SLB_alloc = false;
		}

out:
	mutex_unlock(&g_ammu_table_set.table_lock);
	return ret;
}

static const struct dev_pm_ops apusys_apummu_pm_cb = {
	.resume  = apusys_apummu_resume,
	.suspend = apusys_apummu_suspend,
};

static struct platform_driver apummu_driver = {
	.probe = apummu_probe,
	.remove = apummu_remove,
	.driver = {
		.name = APUSYS_DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &apusys_apummu_pm_cb,
	},
};

static int apummu_rpmsg_cb(struct rpmsg_device *rpdev, void *data,
				 int len, void *priv, u32 src)
{
	int ret = 0;
	void *drvinfo;

	if (!rpdev) {
		AMMU_LOG_ERR("No apummu rpmsg device\n");
		return -1;
	}

	drvinfo = dev_get_drvdata(&rpdev->dev);
	if (!drvinfo) {
		AMMU_LOG_ERR("No apummu dev info\n");
		return -1;
	}

	ret = apummu_remote_rx_cb(drvinfo, data, len);

	return ret;
}

static int apummu_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct apummu_dev_info *adv;
	int ret = 0;

	AMMU_LOG_INFO("name=%s, src=%d\n", rpdev->id.name, rpdev->src);

	if (!g_adv) {
		AMMU_LOG_ERR("No apummu Driver Init\n");
		return -ENODEV;
	}

	adv = g_adv;
	adv->rpdev = rpdev;
	apummu_remote_init();

	dev_set_drvdata(&rpdev->dev, adv);

	mem_task = kthread_run(apummu_rprmsg_memory_func, adv, "apummu");
	if (mem_task == NULL) {
		AMMU_LOG_ERR("create kthread(mem) fail\n");
		ret = -ENOMEM;
		goto out;
	}

	AMMU_LOG_INFO("Done\n");
out:
	return ret;
}

static void apummu_rpmsg_remove(struct rpmsg_device *rpdev)
{
	apummu_remote_exit();

	AMMU_LOG_INFO("Done\n");
}

static const struct of_device_id apummu_rpmsg_of_match[] = {
	{ .compatible = "mediatek,apu-apummu-rpmsg", },
	{ },
};

static struct rpmsg_driver apummu_rpmsg_driver = {
	.drv	= {
		.name	= "apu-apummu-rpmsg",
		.of_match_table = apummu_rpmsg_of_match,
	},
	.probe	= apummu_rpmsg_probe,
	.remove	= apummu_rpmsg_remove,
	.callback = apummu_rpmsg_cb,
};

int apummu_init(void)
{
	int ret = 0;

	apummu_driver.driver.of_match_table = apummu_get_of_device_id();

	ret = platform_driver_register(&apummu_driver);
	if (ret) {
		AMMU_LOG_ERR("failed to register APUSYS driver");
		goto out;
	}

	ret = register_rpmsg_driver(&apummu_rpmsg_driver);
	if (ret) {
		AMMU_LOG_ERR("failed to register RMPSG driver");
		goto out;
	}

	ret = register_rpmsg_driver(&apu_ipi_apummu_rx_rpmsg_driver);
	if (ret) {
		AMMU_LOG_ERR("failed to register RX_RMPSG driver");
		goto out;
	}

	apummu_suspend_release_flag = false;

out:
	return ret;
}

void apummu_exit(void)
{
	unregister_rpmsg_driver(&apu_ipi_apummu_rx_rpmsg_driver);
	unregister_rpmsg_driver(&apummu_rpmsg_driver);
	platform_driver_unregister(&apummu_driver);
}

MODULE_SOFTDEP("pre: mtk_apu_top");

module_init(apummu_init);
module_exit(apummu_exit);
MODULE_DESCRIPTION("APU APUMMU Driver");
MODULE_LICENSE("GPL");
