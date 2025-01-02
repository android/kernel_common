// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/interrupt.h>

#include <linux/firmware/mediatek/mtk-apu.h>
#include <linux/remoteproc/mtk_apu.h>
#include "mtk_apu_rproc.h"

static irqreturn_t mtk_apu_wdt_isr(int irq, void *private_data)
{
	struct mtk_apu *apu = (struct mtk_apu *) private_data;
	struct device *dev = apu->dev;

	if (apu->platdata->flags.secure_coredump) {
		mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_CG_GATING, 0);
		mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_DISABLE_WDT_ISR, 0);
		mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_CLEAR_WDT_ISR, 0);
	} else {
		dev_err(dev, "%s: Not support in non-secure coredump\n", __func__);
	}

	disable_irq_nosync(apu->wdt_irq_number);
	dev_dbg(dev, "%s: disable wdt_irq(%d)\n", __func__, apu->wdt_irq_number);

	/* Bypass the power off timeout checking after the exception is triggered */
	apu->bypass_pwr_off_chk = true;

	return IRQ_HANDLED;
}

static int mtk_apu_wdt_irq_register(struct platform_device *pdev, struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	int ret = 0;

	apu->wdt_irq_number = platform_get_irq(pdev, 0);
	dev_dbg(dev, "%s: wdt_irq_number = %d\n", __func__, apu->wdt_irq_number);

	ret = devm_request_irq(&pdev->dev, apu->wdt_irq_number,
				mtk_apu_wdt_isr, 0, "apusys_wdt", apu);

	if (ret < 0)
		dev_err(dev, "%s: devm_request_irq Failed to request irq %d: %d\n",
				__func__, apu->wdt_irq_number, ret);

	return ret;
}

int mtk_apu_exception_init(struct platform_device *pdev, struct mtk_apu *apu)
{
	int ret = 0;

	ret = mtk_apu_wdt_irq_register(pdev, apu);
	if (ret < 0)
		return ret;

	return ret;
}

void mtk_apu_exception_exit(struct platform_device *pdev, struct mtk_apu *apu)
{
	struct device *dev = apu->dev;

	if (apu->platdata->flags.secure_coredump) {
		mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_DISABLE_WDT_ISR, 0);
		mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_RV_CLEAR_WDT_ISR, 0);
	} else {
		dev_err(dev, "%s: Not support in non-secure boot\n", __func__);
	}

	devm_free_irq(&pdev->dev, apu->wdt_irq_number, "apusys_wdt");
	dev_dbg(dev, "%s: free wdt_irq(%d)\n", __func__, apu->wdt_irq_number);
}
