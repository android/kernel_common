// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/dma-mapping.h>
#include <linux/dev_printk.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <linux/firmware/mediatek/mtk-apu.h>
#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_hw_logger.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include "mtk_apu_rproc.h"

#define MTK_APU_RUN_WAIT_CNT	(3)
#define MTK_APU_RUN_TIMEOUT	(10000)

static const struct reg_field mtk_apu_config_reg_field = {
	.reg = MBOX_HOST_CONFIG_ADDR,
	.lsb = 0,
	.msb = 31,
};

uint32_t mtk_apu_rv_smc_call(struct device *dev, uint32_t smc_id, uint32_t a2)
{
	struct arm_smccc_res res;

	dev_dbg(dev, "%s: smc call %d(a2 = %d)\n", __func__, smc_id, a2);

	arm_smccc_smc(MTK_SIP_APUSYS_CONTROL, smc_id, a2, 0, 0, 0, 0, 0, &res);
	if (((int) res.a0) < 0)
		dev_err(dev, "%s: smc call %d return error(%ld)\n", __func__, smc_id, res.a0);

	return res.a0;
}

static void mtk_apu_dram_boot_remove(struct mtk_apu *apu)
{
	void *domain = iommu_get_domain_for_dev(apu->dev);
	u32 boundary = (u32) upper_32_bits(apu->code_da);
	u64 iova = CODE_BUF_DA | ((u64) boundary << 32);

	if (apu->platdata->flags.secure_boot && !apu->platdata->flags.map_iova)
		return;

	if (domain != NULL) {
		if (apu->platdata->flags.preload_firmware)
			iommu_unmap(domain, MTK_APU_SEC_FW_IOVA, apu->apusys_sec_mem_size);
		else if (!apu->platdata->flags.bypass_iommu)
			iommu_unmap(domain, iova, apu->up_code_buf_sz);
	}

	if (!apu->platdata->flags.preload_firmware)
		dma_free_coherent(apu->dev, apu->up_code_buf_sz, apu->code_buf, apu->code_da);
}

static int mtk_apu_dram_boot_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret = 0;
	void *domain;

	if (apu->platdata->flags.secure_boot && !apu->platdata->flags.map_iova)
		return 0;

	if (!apu->platdata->flags.preload_firmware || apu->platdata->flags.bypass_iommu) {
		dev_err(dev, "%s: not support non-preload firmware\n", __func__);
		return -EINVAL;
	}

	if (!apu->platdata->flags.bypass_iommu) {
		domain = iommu_get_domain_for_dev(apu->dev);
		if (domain == NULL) {
			dev_err(dev, "%s: iommu_get_domain_for_dev fail\n", __func__);
			return -ENOMEM;
		}
	}

	apu->code_buf = (void *) apu->apu_sec_mem_base + apu->apusys_sec_info->up_code_buf_ofs;
	apu->code_da = MTK_APU_SEC_FW_IOVA;
	if (!apu->platdata->flags.smmu_support)
		ret = iommu_map(domain, MTK_APU_SEC_FW_IOVA, apu->apusys_sec_mem_start,
			apu->apusys_sec_mem_size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	else
		ret = iommu_map(domain, MTK_APU_SEC_FW_IOVA, apu->apusys_sec_mem_start,
			apu->apusys_sec_mem_size, IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV,
			GFP_KERNEL);

	if (ret) {
		dev_err(dev, "%s: iommu_map fail(%d)\n", __func__, ret);
		return ret;
	}

	dev_dbg(dev, "%s: iommu_map done\n", __func__);
	return ret;
}

static int mtk_apu_setup_sec_mem(struct mtk_apu *apu)
{
	if (!apu->platdata->flags.secure_boot) {
		dev_err(apu->dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_SETUP_SEC_MEM, 0);
}

static void *mtk_apu_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	void *ptr = NULL;
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;

	if (da < DRAM_OFFSET + apu->up_code_buf_sz) {
		ptr = apu->code_buf + (da - DRAM_OFFSET);
		dev_dbg(apu->dev, "%s: (DRAM): da = 0x%llx, len = 0x%zx\n", __func__, da, len);
	} else if (da >= TCM_OFFSET && da < TCM_OFFSET + apu->platdata->config.up_tcm_sz) {
		if (apu->md32_tcm)
			ptr = apu->md32_tcm + (da - TCM_OFFSET);
		dev_dbg(apu->dev, "%s: (TCM): da = 0x%llx, len = 0x%zx\n", __func__, da, len);
	}
	return ptr;
}

static int __mtk_apu_run(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;
	struct device *dev = apu->dev;
	struct mtk_apu_run *run = &apu->run;
	struct timespec64 begin, end, delta;
	int wait_cnt = MTK_APU_RUN_WAIT_CNT;
	int ret;

	if (!hw_ops->start) {
		WARN_ON(1);
		return -EINVAL;
	}

	ret = hw_ops->setup(apu);
	if (ret)
		goto stop;

	ret = hw_ops->start(apu);
	if (ret) {
		dev_err(dev, "failed to start APU\n");
		goto stop;
	}

	while (wait_cnt > 0) {
		/* check if boot success */
		ktime_get_ts64(&begin);

		ret = wait_event_interruptible_timeout(run->wq, run->signaled,
						       msecs_to_jiffies(MTK_APU_RUN_TIMEOUT));

		ktime_get_ts64(&end);

		if (ret == 0) {
			dev_err(dev, "APU initialization timeout!!\n");
			apu->bypass_pwr_off_chk = true;
			goto stop;
		} else if (ret == -ERESTARTSYS) {
			dev_info(dev, "wait APU interrupted by a signal\n");
			wait_cnt -= 1;
		} else {
			break;
		}
	}

	if (wait_cnt == 0) {
		dev_err(dev, "failed to wait APU interrupt\n");
		apu->bypass_pwr_off_chk = true;
		goto stop;
	}

	delta = timespec64_sub(end, begin);
	dev_dbg(dev, "APU uP boot done. boot time: %llu s, %llu ns. fw_ver: %s\n",
			 (uint64_t)delta.tv_sec, (uint64_t)delta.tv_nsec, run->fw_ver);

	return 0;

stop:
	if (!hw_ops->stop) {
		WARN_ON(1);
		return -EINVAL;
	}
	hw_ops->stop(apu);

	return ret;
}

static int mtk_apu_start(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	int ret;

	if (!(apu->platdata->flags.secure_boot) || (apu->platdata->flags.map_iova))
		apu->apusys_sec_info = apu->apu_sec_mem_base + apu->up_code_buf_sz;

	ret = mtk_apu_config_setup(apu);
	if (ret)
		goto out_mtk_apu_start;

	ret = mtk_apu_dram_boot_init(apu);
	if (ret)
		goto remove_mtk_apu_config_setup;

	ret = mtk_apu_setup_sec_mem(apu);
	if (ret)
		goto remove_mtk_apu_dram_boot;

	dev_dbg(apu->dev, "%s: try to boot uP\n", __func__);
	ret = __mtk_apu_run(rproc);
	if (ret)
		goto remove_mtk_apu_dram_boot;

	devm_iounmap(apu->dev, apu->resv_mem_va);

	return ret;

remove_mtk_apu_dram_boot:
	mtk_apu_dram_boot_remove(apu);

remove_mtk_apu_config_setup:
	mtk_apu_config_remove(apu);

out_mtk_apu_start:
	return ret;
}

static int mtk_apu_stop(struct rproc *rproc)
{
	struct mtk_apu *apu = (struct mtk_apu *)rproc->priv;
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;
	int ret;

	if (!hw_ops->stop) {
		WARN_ON(1);
		return -EINVAL;
	}
	ret = hw_ops->stop(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to stop APU\n");
		return ret;
	}

	mtk_apu_dram_boot_remove(apu);
	mtk_apu_config_remove(apu);

	return 0;
}

static int mtk_apu_prepare(struct rproc *rproc)
{
	struct mtk_apu *apu = rproc->priv;
	struct device_node *data_np;
	struct resource r;
	int ret = 0;
	const struct mtk_apu_platdata *data;
	const struct mtk_apu_hw_ops *hw_ops;

	data = of_device_get_match_data(apu->dev);
	hw_ops = &data->ops;

	/* get reserved memory */
	data_np = of_parse_phandle(apu->pdev->dev.of_node, "memory-region", 0);
	if (!data_np) {
		dev_err(apu->dev, "No reserved memory region found.\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(data_np, 0, &r);
	if (ret) {
		dev_err(apu->dev, "failed to parse reserved memory: %d\n", ret);
		of_node_put(data_np);
		return ret;
	}
	of_node_put(data_np);

	apu->resv_mem_pa = r.start;
	apu->resv_mem_va = devm_ioremap_wc(apu->dev, apu->resv_mem_pa, resource_size(&r));
	dev_dbg(apu->dev, "Reserved memory %pR mapped to %p\n\n", &r, apu->resv_mem_va);
	memset_io(apu->resv_mem_va, 0, resource_size(&r));

	ret = hw_ops->mtk_apu_memmap_init(apu);
	if (ret)
		return ret;

	ret = mtk_apu_mem_init(apu);
	if (ret)
		return ret;

	ret = mtk_apu_ipi_init(apu->pdev, apu);
	if (ret)
		return ret;

	ret = mtk_apu_hw_logger_ipi_init(apu->hw_logger_data, apu);
	if (ret)
		goto remove_mtk_apu_ipi;

	ret = mtk_apu_timesync_init(apu);
	if (ret)
		goto remove_apu_hw_logger_ipi;

	ret = mtk_apu_exception_init(apu->pdev, apu);
	if (ret)
		goto remove_mtk_apu_timesync;

	if (hw_ops->init) {
		ret = hw_ops->init(apu);
		if (ret)
			goto remove_mtk_apu_exception;
	}

	return ret;

remove_mtk_apu_exception:
	mtk_apu_exception_exit(apu->pdev, apu);

remove_mtk_apu_timesync:
	mtk_apu_timesync_remove(apu);

remove_apu_hw_logger_ipi:
	mtk_apu_hw_logger_ipi_remove(apu);

remove_mtk_apu_ipi:
	mtk_apu_ipi_remove(apu);

	return ret;
}

static int mtk_apu_unprepare(struct rproc *rproc)
{
	struct mtk_apu *apu = rproc->priv;

	mtk_apu_exception_exit(apu->pdev, apu);
	mtk_apu_timesync_remove(apu);
	mtk_apu_hw_logger_ipi_remove(apu);
	mtk_apu_ipi_remove(apu);

	return 0;
}

static const struct rproc_ops mtk_apu_ops = {
	.start		= mtk_apu_start,
	.stop		= mtk_apu_stop,
	.load		= mtk_apu_load,
	.prepare	= mtk_apu_prepare,
	.unprepare	= mtk_apu_unprepare,
	.da_to_va	= mtk_apu_da_to_va,
};

struct regmap *mtk_apu_get_mbox_regmap(struct device_node *node)
{
	struct device_node *mbox_node;
	struct platform_device *mbox_pdev;
	struct regmap *regmap = NULL;

	mbox_node = of_parse_phandle(node, "mediatek,mbox-spare-reg", 0);
	if (!mbox_node)
		return ERR_PTR(-ENODEV);

	mbox_pdev = of_find_device_by_node(mbox_node);
	if (!mbox_pdev) {
		regmap = ERR_PTR(-EPROBE_DEFER);
		goto out_put_node;
	}

	regmap = dev_get_regmap(&mbox_pdev->dev, NULL);
	if (!regmap)
		regmap = ERR_PTR(-EINVAL);

	platform_device_put(mbox_pdev);

out_put_node:
	of_node_put(mbox_node);
	return regmap;
}

static int mtk_apu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *hw_logger_np, *apu_power_np;
	struct device_link *link;
	struct rproc *rproc;
	struct mtk_apu *apu;
	const struct mtk_apu_platdata *data;
	const struct mtk_apu_hw_ops *hw_ops;
	struct regmap *regmap;
	int ret = 0;

	data = of_device_get_match_data(dev);
	hw_ops = &data->ops;

	rproc = devm_rproc_alloc(dev, np->name, &mtk_apu_ops, data->fw_name, sizeof(struct mtk_apu));
	if (!rproc)
		return dev_err_probe(dev, -ENOMEM, "unable to allocate remoteproc\n");

	rproc->auto_boot = data->flags.auto_boot;
	rproc->sysfs_read_only = true;

	apu = rproc->priv;
	apu->rproc = rproc;
	apu->pdev = pdev;
	apu->dev = dev;
	apu->platdata = data;
	platform_set_drvdata(pdev, apu);

	apu_power_np = of_parse_phandle(np, "mediatek,apu-power", 0);
	if (!apu_power_np)
		dev_err(dev, "failed to get mediatek,apu-power phandle\n");

	apu->power_pdev = of_find_device_by_node(apu_power_np);
	of_node_put(apu_power_np);
	if (!apu->power_pdev)
		return dev_err_probe(dev, -EPROBE_DEFER, "failed to find power device for mtk_apu\n");

	link = device_link_add(dev, &apu->power_pdev->dev, DL_FLAG_PM_RUNTIME);
	if (!link)
		dev_err(dev, "Unable to create device link between mtk_apu and mtk apu power\n");

	hw_logger_np = of_parse_phandle(np, "mediatek,hw-logger", 0);
	if (!hw_logger_np)
		dev_err(dev, "failed to get hw-logger phandle\n");

	apu->hw_logger_data = get_mtk_apu_hw_logger_device(hw_logger_np);

	of_node_put(hw_logger_np);

	if (!apu->hw_logger_data || IS_ERR(apu->hw_logger_data)) {
		dev_err(dev, "failed to get hw-logger data\n");
		ret = PTR_ERR(apu->hw_logger_data);
		return dev_err_probe(dev, ret, "failed to get mtk_apu_hw_logger device: %d\n",
				     ret);
	}

	regmap = mtk_apu_get_mbox_regmap(dev->of_node);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		return dev_err_probe(dev, ret, "failed to get mailbox regmap: %d\n", ret);
	}

	apu->config_regmap_field = devm_regmap_field_alloc(dev, regmap, mtk_apu_config_reg_field);
	if (IS_ERR(apu->config_regmap_field)) {
		ret = PTR_ERR(apu->config_regmap_field);
		return dev_err_probe(dev, ret, "failed to allocate config regmap field: %d\n",
				     ret);
	}

	mtk_apu_init_mdw_dev(dev);

	spin_lock_init(&apu->reg_lock);

	if (!hw_ops->mtk_apu_memmap_init) {
		dev_err(dev, "%s: mtk_apu_memmap_init is not implemented\n", __func__);
		return -EOPNOTSUPP;
	}

	ret = rproc_add(rproc);
	if (ret < 0) {
		dev_err(dev, "boot fail ret:%d\n", ret);
		goto err_put_device;
	}

err_put_device:
	return ret;
}

static int mtk_apu_resume(struct device *dev)
{
	struct mtk_apu *apu = dev_get_drvdata(dev);
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;

	if (!hw_ops->resume)
		return 0;

	return hw_ops->resume(apu);
}

static int mtk_apu_suspend(struct device *dev)
{
	struct mtk_apu *apu = dev_get_drvdata(dev);
	const struct mtk_apu_hw_ops *hw_ops = &apu->platdata->ops;

	if (!hw_ops->suspend)
		return 0;

	return hw_ops->suspend(apu);
}

extern const struct mtk_apu_platdata mt8196_platdata;
static const struct of_device_id mtk_apu_of_match[] = {
	{ .compatible = "mediatek,mt8196-apusys_rv", .data = &mt8196_platdata },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_apu_of_match);

static const struct dev_pm_ops mtk_apu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_apu_suspend, mtk_apu_resume)
};

/*
 * The kernel driver of the MediaTek APU will load the image into a specific
 * reserved memory area and then protect the memory immediately after the loading is completed.
 * Once the protection is set up, the kernel cannot access the memory to prevent
 * non-secure operations.
 * Hence, we do not support module removal in this driver because we cannot probe the APU again
 * once the memory protection has been established.
 */
static struct platform_driver mtk_apu_driver = {
	.probe = mtk_apu_probe,
	.driver = {
		.name = "mtk-apu",
		.pm = &mtk_apu_pm_ops,
		.suppress_bind_attrs = true,
		.of_match_table = of_match_ptr(mtk_apu_of_match),
	},
};

int mtk_apu_rproc_init(void)
{
	int ret;

	ret = mtk_apu_hw_logger_init();
	if (ret) {
		pr_err("failed to init hw logger\n");
		return ret;
	}

	ret = platform_driver_register(&mtk_apu_driver);
	if (ret)
		pr_err("failed to register mtk_apu_driver\n");

	return ret;
}

MODULE_IMPORT_NS(MTK_APU_PWR);
MODULE_IMPORT_NS(MTK_APU_MDW);
MODULE_IMPORT_NS(MTK_APU_MVPU);

MODULE_SOFTDEP("pre: mtk-apu-mailbox");

module_init(mtk_apu_rproc_init);
MODULE_DESCRIPTION("MTK APUSys Driver");
MODULE_LICENSE("GPL");
