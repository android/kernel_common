// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/firmware/mediatek/mtk-apu.h>
#include <linux/io.h>
#include <linux/remoteproc/mtk_apu.h>
#include <linux/soc/mediatek/mtk_apu_pwr.h>
#include "../mtk_apu_rproc.h"



static int apu_setup_apummu(struct mtk_apu *apu)
{
	if (!(apu->platdata->flags.secure_boot)) {
		dev_err(apu->dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_SETUP_APUMMU, 0);
}

static int apu_setup_devapc(struct mtk_apu *apu)
{
	return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_DEVAPC_INIT_RCX, 0);
}

static int apu_reset_mp(struct mtk_apu *apu)
{
	if (!(apu->platdata->flags.secure_boot)) {
		dev_err(apu->dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_RESET_MP, 0);
}

static int apu_setup_boot(struct mtk_apu *apu)
{
	if (!(apu->platdata->flags.secure_boot)) {
		dev_err(apu->dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_SETUP_BOOT, 0);
}

static int mt8196_rproc_start(struct mtk_apu *apu)
{
	if (!(apu->platdata->flags.secure_boot)) {
		dev_err(apu->dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_START_MP, 0);
}

static int mt8196_rproc_setup(struct mtk_apu *apu)
{
	int ret;

	ret = apu_setup_devapc(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to setup devapc\n");
		return ret;
	}

	ret = apu_setup_apummu(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to setup apummu\n");
		return ret;
	}

	ret = apu_reset_mp(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to reset mp\n");
		return ret;
	}

	ret = apu_setup_boot(apu);
	if (ret) {
		dev_err(apu->dev, "Failed to setup boot\n");
		return ret;
	}

	return ret;
}

static int mt8196_rproc_stop(struct mtk_apu *apu)
{
	if (!(apu->platdata->flags.secure_boot)) {
		dev_err(apu->dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_STOP_MP, 0);
}

static int mt8196_apu_memmap_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;

	apu->md32_tcm = NULL;

	apu->apu_infra_hwsem = devm_ioremap(dev, 0x190b0e00, 0xff);
	if (IS_ERR((void const *)apu->apu_infra_hwsem)) {
		dev_err(dev, "%s: apu_infra_hwsem remap base fail\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

static int mt8196_rproc_init(struct mtk_apu *apu)
{
	apu->is_under_lp_scp_recovery_flow = false;
	return 0;
}

static int mt8196_apu_resume(struct mtk_apu *apu)
{
	mutex_lock(&apu->forbid_ipi_lock);
	apu->forbid_ipi_send = false;
	mutex_unlock(&apu->forbid_ipi_lock);

	return 0;
}

static int mt8196_apu_suspend(struct mtk_apu *apu)
{
	int pwr_status = mtk_apu_get_rpc_pwr_status(apu->power_pdev) & 0x1;

	if (pwr_status) {
		// Deny any incoming IPI
		mutex_lock(&apu->forbid_ipi_lock);
		apu->forbid_ipi_send = true;
		mutex_unlock(&apu->forbid_ipi_lock);

		// Cancel current timer and do power off if needed
		if (timer_pending(&apu->power_off_timer))
			del_timer(&apu->power_off_timer);
	}

	return 0;
}

const struct mtk_apu_platdata mt8196_platdata = {
	.flags	= {
		.auto_boot = true,
		.fast_on_off = true,
		.infra_wa = true,
		.kernel_load_image = true,
		.map_iova = true,
		.preload_firmware = true,
		.secure_boot = true,
		.secure_coredump = true,
		.smmu_support = true,
	},
	.config = {
		.up_code_buf_sz = 0x100000,
		.up_coredump_buf_sz = 0x160000,
		.regdump_buf_sz	= 0x10000,
		.mdla_coredump_buf_sz = 0x0,
		.mvpu_coredump_buf_sz = 0x0,
		.mvpu_sec_coredump_buf_sz = 0x0,
		.up_tcm_sz = 0x50000,
		.ce_coredump_buf_sz = 0x10000
	},
	.ops		= {
		.init	= mt8196_rproc_init,
		.start	= mt8196_rproc_start,
		.setup = mt8196_rproc_setup,
		.stop	= mt8196_rproc_stop,
		.mtk_apu_memmap_init = mt8196_apu_memmap_init,
		.suspend = mt8196_apu_suspend,
		.resume = mt8196_apu_resume,
	},
	.fw_name = "mediatek/mt8196/apusys.img",
};
