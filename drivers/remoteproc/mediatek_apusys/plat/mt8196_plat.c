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

enum apu_infra_bit_id {
	APU_INFRA_SYS_APMCU = 1UL,
	APU_INFRA_SYS_GZ = 2UL,
	APU_INFRA_SYS_SCP = 3UL,
};

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

int apu_infra_lock(struct mtk_apu *apu, uint32_t op, enum apu_infra_bit_id id)
{
	uint32_t timeout_cnt = 0;
	uint32_t timeout = 1000000;
	struct device *dev = apu->dev;

	if (op == 1)
		iowrite32(BIT(id), apu->apu_infra_hwsem);
	else if (op == 0)
		iowrite32(BIT(id + 16), apu->apu_infra_hwsem);

	if (op == 0)
		goto end;

	while ((ioread32(apu->apu_infra_hwsem) & BIT(id)) != BIT(id)) {
		if (timeout_cnt++ >= timeout) {
			dev_err(dev, "%s: apu_infra_hwsem :0x%08x\n", __func__,
				ioread32(apu->apu_infra_hwsem));
			return -EBUSY;
		}

		iowrite32(BIT(id), apu->apu_infra_hwsem);
		udelay(1);
	}
end:
	return 0;
}

static int apu_power_ctrl(struct mtk_apu *apu, uint32_t op)
{
	if (!(apu->platdata->flags.secure_boot)) {
		dev_err(apu->dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	if (op == 1)
		return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_PWR_TOP_ON, 0);
	else if (op == 0)
		return mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_PWR_TOP_OFF, 0);
	else
		return -EINVAL;
}

static int mt8196_cold_boot_power_on(struct mtk_apu *apu)
{
	int ret;
	struct device *dev = apu->dev;

	if (!(apu->platdata->flags.secure_boot)) {
		dev_err(dev, "Not support in non-secure boot\n");
		return -EINVAL;
	}

	apu->ipi_pwr_ref_cnt[MTK_APU_IPI_INIT]++;
	apu->local_pwr_ref_cnt++;

	ret = mtk_apu_rv_smc_call(dev, MTK_APUSYS_KERNEL_OP_APUSYS_COLD_BOOT_CLR_MBOX_DUMMY, 0);
	if (ret) {
		dev_err(dev, "Failed to clear mailbox dummy register, ret=%d\n", ret);
		return ret;
	}

	ret = apu_power_ctrl(apu, 1);
	if (ret)
		dev_err(dev, "Failed to power on APU, ret=%d\n", ret);

	return ret;
}

static int mt8196_power_on_off_locked(struct mtk_apu *apu, u32 id, u32 on, u32 off)
{
	int ret = 0;
	struct device *dev = apu->dev;
	uint32_t rpc_state = 0;

	lockdep_assert_held(&apu->power_lock);

	if (on == 1 && off == 0) {
		if (apu->is_under_lp_scp_recovery_flow)
			return -EBUSY;

		if (apu->ipi_pwr_ref_cnt[id] < U32_MAX) {
			apu->ipi_pwr_ref_cnt[id]++;
			apu->local_pwr_ref_cnt++;

			if (apu->local_pwr_ref_cnt == 1) {
				rpc_state = mtk_apu_get_rpc_status(apu->power_pdev) & 0x1;
				if (id != MTK_APU_IPI_SCP_NP_RECOVER && rpc_state == 1) {
					dev_warn(dev, "%s: APU RPC is under LP mode, retry later\n", __func__);
					return -EBUSY;
				}

				ret = apu_power_ctrl(apu, 1);
				if (!ret) {
					if (id == MTK_APU_IPI_SCP_NP_RECOVER && rpc_state == 1)
						apu->is_under_lp_scp_recovery_flow = true;
				} else {
					apu->ipi_pwr_ref_cnt[id]--;
					apu->local_pwr_ref_cnt--;
					dev_err(dev, "%s: APU power on fail(%d)\n", __func__, ret);
				}
			}
		} else {
			dev_err(dev, "%s: ipi_pwr_ref_cnt[%u] == U32_MAX\n", __func__, id);
			ret = -EINVAL;
		}
	} else if (on == 0 && off == 1) {
		if (apu->ipi_pwr_ref_cnt[id] != 0) {
			apu->ipi_pwr_ref_cnt[id]--;
			apu->local_pwr_ref_cnt--;

			if (apu->local_pwr_ref_cnt == 0) {
				if (apu->platdata->flags.infra_wa)
					apu_infra_lock(apu, 1, APU_INFRA_SYS_APMCU);

				ret = apu_power_ctrl(apu, 0);

				if (apu->platdata->flags.infra_wa)
					apu_infra_lock(apu, 0, APU_INFRA_SYS_APMCU);

				if (!ret) {
					if (id == MTK_APU_IPI_SCP_NP_RECOVER)
						apu->is_under_lp_scp_recovery_flow = false;
				} else {
					apu->ipi_pwr_ref_cnt[id]++;
					apu->local_pwr_ref_cnt++;
					dev_err(dev, "%s: APU power off fail(%d)\n", __func__, ret);
				}
			}
		} else {
			dev_err(dev, "%s: ipi_pwr_ref_cnt[%u] == 0\n", __func__, id);
			ret = -EINVAL;
		}
	} else {
		dev_err(dev, "%s: invalid operation: id(%d), on(%d), off(%d)\n",
			__func__, id, on, off);
		ret = -EINVAL;
	}

	return ret;
}

static int mt8196_power_on_off(struct mtk_apu *apu, u32 id, u32 on, u32 off)
{
	int ret = 0;
	struct device *dev = apu->dev;
	uint32_t retry_cnt = 500, i = 0;

	for (i = 0; i < retry_cnt; i++) {
		mutex_lock(&apu->power_lock);

		ret = mt8196_power_on_off_locked(apu, id, on, off);

		mutex_unlock(&apu->power_lock);

		if (ret == -EBUSY) {
			/*
			 * Retry the power on/off if the returned value is -EBUSY, because
			 * the hw semaphore might be blocked by other host or apu under lp mode
			 */
			if (i!=0 && (i%10)==0)
				dev_warn(dev, "%s: retry on(%u) off(%u)(%u/%u)\n", __func__,
					 on, off, i, retry_cnt);
			if (i < 10)
				usleep_range(200, 500);
			else if (i < 50)
				usleep_range(1000, 2000);
			else
				usleep_range(10000, 11000);
			continue;
		}
		break;
	}

	return ret;
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
	int ret;

	apu->is_under_lp_scp_recovery_flow = false;

	ret = mt8196_cold_boot_power_on(apu);
	if (ret)
		dev_err(apu->dev, "%s: call mt8196_cold_boot_power_on fail(%d)\n",
			__func__, ret);

	return ret;
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
		if (timer_pending(&apu->power_off_timer)) {
			del_timer(&apu->power_off_timer);
			mt8196_power_on_off(apu, MTK_APU_IPI_MIDDLEWARE, 0, 1);
		}
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
		.power_on_off = mt8196_power_on_off,
		.suspend = mt8196_apu_suspend,
		.resume = mt8196_apu_resume,
	},
	.fw_name = "mediatek/mt8196/apusys.img",
};
