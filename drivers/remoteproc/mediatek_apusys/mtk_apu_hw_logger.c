// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <asm/div64.h>
#include <asm/io.h>
#include <linux/arm-smccc.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/sched/clock.h>
#include <linux/syscalls.h>

#include <linux/firmware/mediatek/mtk-apu.h>
#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/remoteproc/mtk_apu_hw_logger.h>
#include <linux/mailbox/mtk-apu-mailbox.h>
#include <linux/soc/mediatek/mtk_apu_pwr.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#include "mtk_apu_rproc.h"

#define APUSYS_HWLOGR_DIR	"apusys_logger"
#define HWLOG_DEV_NAME		"apu_hw_logger"

static const struct reg_field mtk_apu_log_r_reg_field = {
	.reg = LOG_R_OFS,
	.lsb = 0,
	.msb = 31,
};

static const struct reg_field mtk_apu_log_w_reg_field = {
	.reg = LOG_W_OFS,
	.lsb = 0,
	.msb = 31,
};

static void hw_logger_buf_invalidate(struct mtk_apu_hw_logger *hw_logger_data)
{
	dma_sync_single_for_cpu(hw_logger_data->apu->dev,
				hw_logger_data->hw_log_buf.hw_log_buf_dma_addr,
				HWLOG_LOG_SIZE, DMA_FROM_DEVICE);
}

static int hw_logger_buf_alloc(struct mtk_apu_hw_logger *hw_logger_data)
{
	int ret = 0;

	ret = dma_set_coherent_mask(hw_logger_data->apu->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(hw_logger_data->dev, "dma_set_coherent_mask fail (%d)\n", ret);
		ret = -ENOMEM;
		goto out;
	}

	/* local buffer for dump */
	hw_logger_data->local_log_buf = kzalloc(LOCAL_LOG_SIZE, GFP_KERNEL);
	if (!hw_logger_data->local_log_buf) {
		ret = -ENOMEM;
		goto out;
	}

	/* memory used by hw logger */
	hw_logger_data->hw_log_buf.hw_log_buf = kzalloc(HWLOG_LOG_SIZE, GFP_KERNEL);
	if (!hw_logger_data->hw_log_buf.hw_log_buf) {
		ret = -ENOMEM;
		goto out;
	}

	hw_logger_data->hw_log_buf.hw_log_buf_dma_addr = dma_map_single(hw_logger_data->apu->dev,
		hw_logger_data->hw_log_buf.hw_log_buf, HWLOG_LOG_SIZE, DMA_FROM_DEVICE);
	ret = dma_mapping_error(hw_logger_data->apu->dev,
		hw_logger_data->hw_log_buf.hw_log_buf_dma_addr);
	if (ret) {
		dev_err(hw_logger_data->dev,
			"dma_map_single fail for hw_log_buf_dma_addr (%d)\n", ret);
		ret = -ENOMEM;
		goto out;
	}

	dev_dbg(hw_logger_data->dev, "local_log_buf = %p\n", hw_logger_data->local_log_buf);
	dev_dbg(hw_logger_data->dev, "hw_log_buf = %pK, hw_log_buf_dma_addr = %pad\n",
		hw_logger_data->hw_log_buf.hw_log_buf,
		&hw_logger_data->hw_log_buf.hw_log_buf_dma_addr);

out:
	return ret;
}

static void hw_logger_buf_free(struct device *dev, struct mtk_apu_hw_logger *hw_logger_data)
{
	dma_unmap_single(dev, hw_logger_data->hw_log_buf.hw_log_buf_dma_addr, HWLOG_LOG_SIZE,
			 DMA_FROM_DEVICE);

	kfree(hw_logger_data->hw_log_buf.hw_log_buf);
	hw_logger_data->hw_log_buf.hw_log_buf = NULL;
	kfree(hw_logger_data->local_log_buf);
	hw_logger_data->local_log_buf = NULL;
}

static int ioread32_atf(uint8_t op_num, void **addr, uint32_t **ret_val,
			struct mtk_apu_hw_logger *hw_logger_data)
{
	int i, op = 0;
	uint8_t smc_op;
	struct arm_smccc_res res;
	unsigned long flags = 0;
	struct device *dev = hw_logger_data->dev;

	if (op_num == 0 || op_num > MAX_SMC_OP_NUM) {
		dev_err(dev, "op_num invalid: %d\n", op_num);
		return -EINVAL;
	}

	for (i = 0; i < op_num; i++) {
		if (addr[i] == hw_logger_data->apu_logtop + APU_LOG_BUF_T_SIZE_OFF)
			smc_op = SMC_OP_APU_LOG_BUF_T_SIZE;
		else if (addr[i] == hw_logger_data->apu_logtop + APU_LOG_BUF_W_PTR_OFF)
			smc_op = SMC_OP_APU_LOG_BUF_W_PTR;
		else if (addr[i] == hw_logger_data->apu_logtop + APU_LOG_BUF_R_PTR_OFF)
			smc_op = SMC_OP_APU_LOG_BUF_R_PTR;
		else if (addr[i] == hw_logger_data->apu_logtop + APU_LOGTOP_CON_OFF)
			smc_op = SMC_OP_APU_LOG_BUF_CON;
		else {
			dev_err(dev, "Not support addr: 0x%llx", (unsigned long long)addr[i]);
			return -EINVAL;
		}
		op |= smc_op << (8 * i);
	}
	dev_dbg(dev, "arm_smccc_smc reg_dump op: 0x%08x\n", op);

	spin_lock_irqsave(&hw_logger_data->smc_spinlock, flags);
	arm_smccc_smc(MTK_SIP_APUSYS_CONTROL,
		MTK_APUSYS_KERNEL_OP_APUSYS_LOGTOP_REG_DUMP,
		op, 0, 0, 0, 0, 0, &res);
	spin_unlock_irqrestore(&hw_logger_data->smc_spinlock, flags);

	if (res.a0 != 0) {
		if (res.a0 == -16) {
			dev_dbg(dev, "arm_smccc_smc reg_dump acquire rcx sema timeout (rcx off)\n");
		} else {
			dev_err(dev, "arm_smccc_smc reg_dump error ret: 0x%lx\n", res.a0);
			dev_err(dev, "arm_smccc_smc reg_dump op: 0x%08x\n", op);
			dev_err(dev, "arm_smccc_smc reg_dump a0 a1 a2 a3 / 0x%lx 0x%lx 0x%lx 0x%lx\n",
				res.a0, res.a1, res.a2, res.a3);
		}
		return res.a0;
	}

	*ret_val[0] = res.a1;
	if (op_num > 1)
		*ret_val[1] = res.a2;
	if (op_num > 2)
		*ret_val[2] = res.a3;

	return 0;
}

static int iowrite32_atf(uint32_t write_val, void *addr, struct mtk_apu_hw_logger *hw_logger_data)
{
	int op;
	struct arm_smccc_res res;
	unsigned long flags = 0;
	struct device *dev = hw_logger_data->dev;

	if (addr == hw_logger_data->apu_logtop + APU_LOG_BUF_R_PTR_OFF) {
		op = SMC_OP_APU_LOG_BUF_R_PTR;
	} else {
		dev_err(dev, "Not support addr: %p", addr);
		return -EINVAL;
	}

	spin_lock_irqsave(&hw_logger_data->smc_spinlock, flags);
	arm_smccc_smc(MTK_SIP_APUSYS_CONTROL, MTK_APUSYS_KERNEL_OP_APUSYS_LOGTOP_REG_WRITE,
		      op, write_val, 0, 0, 0, 0, &res);
	spin_unlock_irqrestore(&hw_logger_data->smc_spinlock, flags);

	if (res.a0 != 0) {
		if (res.a0 == -16) {
			dev_info(dev, "arm_smccc_smc reg_write acquire rcx sema timeout (rcx off)\n");
		} else {
			dev_err(dev, "arm_smccc_smc reg_write error! ret = 0x%lx, a1 = 0x%lx",
				res.a0, res.a1);
			dev_err(dev, "arm_smccc_smc reg_write op val / 0x%x 0x%x\n", op, write_val);
		}
		return res.a0;
	}

	return 0;
}

static int w1c32_atf(void *addr, uint32_t *ret_val, struct mtk_apu_hw_logger *hw_logger_data)
{
	int op;
	struct arm_smccc_res res;
	unsigned long flags = 0;
	struct device *dev = hw_logger_data->dev;

	if (addr == hw_logger_data->apu_logtop + APU_LOGTOP_CON_OFF) {
		op = SMC_OP_APU_LOG_BUF_CON;
	} else {
		dev_err(dev, "Not support addr: %p", addr);
		return -EINVAL;
	}

	spin_lock_irqsave(&hw_logger_data->smc_spinlock, flags);
	arm_smccc_smc(MTK_SIP_APUSYS_CONTROL,
		MTK_APUSYS_KERNEL_OP_APUSYS_LOGTOP_REG_W1C,
		op, 0, 0, 0, 0, 0, &res);
	spin_unlock_irqrestore(&hw_logger_data->smc_spinlock, flags);

	if (res.a0 != 0) {
		if (res.a0 == -16) {
			dev_info(dev, "arm_smccc_smc reg_w1c acquire rcx sema timeout (rcx off)\n");
		} else {
			dev_err(dev, "arm_smccc_smc reg_w1c error! ret = 0x%lx, a1 = 0x%lx",
				res.a0, res.a1);
			dev_err(dev, "arm_smccc_smc reg_w1c op / 0x%x\n", op);
		}
		return res.a0;
	}

	*ret_val = res.a1;
	return 0;
}

static unsigned long long get_st_addr(struct mtk_apu_hw_logger *hw_logger_data)
{
	return (unsigned long long) hw_logger_data->hw_log_buf.hw_log_buf_dma_addr;
}

static unsigned int get_t_size(void)
{
	return HWLOG_LOG_SIZE;
}

static int get_r_w_ptr(unsigned long long *r_ptr, unsigned long long *w_ptr,
		       struct mtk_apu_hw_logger *hw_logger_data)
{
	int ret = 0;
	uint32_t r_ptr_reg = 0, w_ptr_reg = 0;

	ret = ioread32_atf(2,
		(void*[]){hw_logger_data->apu_logtop + APU_LOG_BUF_R_PTR_OFF,
			  hw_logger_data->apu_logtop + APU_LOG_BUF_W_PTR_OFF},
		(uint32_t*[]){&r_ptr_reg, &w_ptr_reg}, hw_logger_data
	);
	if (ret)
		goto out;

	/* hw log w/r_ptr is a 34bit addr but store in a 32bit feild */
	if (r_ptr != NULL)
		*r_ptr = (unsigned long long)r_ptr_reg << 2;
	if (w_ptr != NULL)
		*w_ptr = (unsigned long long)w_ptr_reg << 2;

out:
	return ret;
}

static void set_r_ptr(unsigned long long r_ptr, struct mtk_apu_hw_logger *hw_logger_data)
{
	int ret = 0;

	ret = iowrite32_atf(lower_32_bits(r_ptr >> 2),
			    hw_logger_data->apu_logtop + APU_LOG_BUF_R_PTR_OFF,
			    hw_logger_data);
	if (ret < 0)
		dev_dbg(hw_logger_data->dev, "iowrite32_atf failed\n");
}

static void store_r_ofs(unsigned int pwr_status, unsigned int r_ofs,
			struct mtk_apu_hw_logger *hw_logger_data)
{
	int ret;

	/* set read pointer */
	if (pwr_status != 0) {
		dev_dbg(hw_logger_data->dev, "set_r_ptr r_ofs = 0x%x\n", r_ofs);
		set_r_ptr(get_st_addr(hw_logger_data) + r_ofs, hw_logger_data);
	}

	dev_dbg(hw_logger_data->dev, "set_r_ofs_mbox r_ofs = 0x%x\n", r_ofs);
	ret = regmap_field_write(hw_logger_data->log_read_regmap_field, r_ofs);
	if (ret)
		dev_err(hw_logger_data->dev, "failed to set r_ofs\n");
}

static struct platform_driver hw_logger_driver;
struct mtk_apu_hw_logger *get_mtk_apu_hw_logger_device(struct device_node *hw_logger_np)
{
	struct platform_device *pdev;
	struct device *dev;
	struct mtk_apu_hw_logger *hw_logger_data;

	pdev = of_find_device_by_node(hw_logger_np);
	if (!pdev)
		return ERR_PTR(-ENODEV);

	dev = &pdev->dev;

	if (!dev->driver)
		return -EPROBE_DEFER;

	if (dev->driver != &hw_logger_driver.driver)
		return -EINVAL;

	hw_logger_data = dev_get_drvdata(dev);
	put_device(dev);

	return hw_logger_data;
}

static void mtk_apu_hw_log_level_ipi_handler(void *data, unsigned int len, void *priv)
{
	int ret;
	unsigned int log_level = *(unsigned int *)data;
	struct mtk_apu *apu = priv;

	pr_info("log_level = 0x%x (%d)\n", log_level, len);
	ret = mtk_apu_power_on_off(apu->pdev, MTK_APU_IPI_LOG_LEVEL, 0, 1);
	if (ret && ret != -EOPNOTSUPP)
		pr_err("mtk_apu_power_on_off fail(%d)\n", ret);
}

dma_addr_t mtk_apu_hw_logger_config_init(struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long flags;

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->g_log.norm_mode.w_ptr = 0;
	hw_logger_data->g_log.norm_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.norm_mode.ov_flg = 0;
	hw_logger_data->g_log.lock_mode.w_ptr = 0;
	hw_logger_data->g_log.lock_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.lock_mode.ov_flg = 0;
	hw_logger_data->g_log.mobile_lock_mode.w_ptr = 0;
	hw_logger_data->g_log.mobile_lock_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.mobile_lock_mode.ov_flg = 0;
	hw_logger_data->buf_track.__loc_log_sz = 0;
	atomic_set(&hw_logger_data->apu_toplog_deep_idle, 0);
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	hw_logger_buf_alloc(hw_logger_data);

	return hw_logger_data->hw_log_buf.hw_log_buf_dma_addr;
}

int mtk_apu_hw_logger_ipi_init(struct mtk_apu_hw_logger *hw_logger_data, struct mtk_apu *apu)
{
	int ret = 0;

	hw_logger_data->apu = apu;

	if (!hw_logger_data->apu_logtop)
		return 0;

	ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_LOG_LEVEL, NULL,
				   mtk_apu_hw_log_level_ipi_handler, apu);
	if (ret)
		dev_err(apu->dev, "Fail in hw_log_level_ipi_init\n");
	return 0;
}

void mtk_apu_hw_logger_ipi_remove(struct mtk_apu *apu)
{
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_LOG_LEVEL);
}

static int __apu_logtop_copy_buf(unsigned int r_ofs, unsigned int w_ofs, unsigned int pwr_status,
				 struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long flags;
	unsigned int t_size, r_size;
	unsigned int log_w_ofs, log_ov_flg;
	int ret = 0;
	static bool err_log;

	if (!hw_logger_data->apu_logtop || !hw_logger_data->hw_log_buf.hw_log_buf
	    || !hw_logger_data->local_log_buf)
		return 0;

	dev_dbg(hw_logger_data->dev, "w_ofs = 0x%x, r_ofs = 0x%x\n", w_ofs, r_ofs);

	if (w_ofs == r_ofs)
		goto out;

	t_size = get_t_size();

	/* check copy size */
	if (w_ofs > r_ofs)
		r_size = w_ofs - r_ofs;
	else
		r_size = t_size - r_ofs + w_ofs;

	/* sanity check */
	if (r_size >= t_size)
		r_size = t_size;

	ret = r_size;

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	log_w_ofs = hw_logger_data->buf_track.__loc_log_w_ofs;
	log_ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	if (w_ofs + HWLOG_LINE_MAX_LENS > t_size || r_ofs + HWLOG_LINE_MAX_LENS > t_size ||
	    t_size == 0 || t_size > HWLOG_LOG_SIZE) {
		if (!err_log)
			dev_warn(hw_logger_data->dev, "w_ofs = 0x%x, r_ofs = 0x%x, t_size = 0x%x\n",
				 w_ofs, r_ofs, t_size);
		err_log = true;
		return 0;
	}

	if (log_w_ofs + HWLOG_LINE_MAX_LENS > LOCAL_LOG_SIZE) {
		if (!err_log)
			dev_warn(hw_logger_data->dev, "log_w_ofs = 0x%x\n", log_w_ofs);
		err_log = true;
		return 0;
	}

	if (err_log) {
		dev_warn(hw_logger_data->dev, "[ok] w_ofs = 0x%x, r_ofs = 0x%x, t_size = 0x%x\n",
			 w_ofs, r_ofs, t_size);
		err_log = false;
	}

	hw_logger_buf_invalidate(hw_logger_data);

	while (r_size > 0) {
		/* copy hw logger buffer to local buffer */
		memcpy(hw_logger_data->local_log_buf + log_w_ofs,
		       hw_logger_data->hw_log_buf.hw_log_buf + r_ofs, HWLOG_LINE_MAX_LENS);

		/* update local write pointer */
		log_w_ofs = (log_w_ofs + HWLOG_LINE_MAX_LENS) % LOCAL_LOG_SIZE;

		/* update hw logger read pointer */
		r_ofs = (r_ofs + HWLOG_LINE_MAX_LENS) % t_size;
		r_size -= HWLOG_LINE_MAX_LENS;

		hw_logger_data->buf_track.__loc_log_sz += HWLOG_LINE_MAX_LENS;
		if (hw_logger_data->buf_track.__loc_log_sz >= LOCAL_LOG_SIZE)
			log_ov_flg = 1;
	};

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->buf_track.__loc_log_w_ofs = log_w_ofs;
	hw_logger_data->buf_track.__loc_log_ov_flg = log_ov_flg;
	hw_logger_data->g_log.lock_mode.not_rd_sz += ret;
	hw_logger_data->g_log.mobile_lock_mode.not_rd_sz += ret;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	store_r_ofs(pwr_status, r_ofs, hw_logger_data);
out:
	return ret;
}

static int apu_logtop_copy_buf(struct mtk_apu_hw_logger *hw_logger_data)
{
	int ret = 0;
	static bool lock_fail;
	unsigned int r_ofs, w_ofs, pwr_status;
	unsigned long long r_ptr = 0, w_ptr = 0;

	if (!mutex_trylock(&hw_logger_data->mutex)) {
		if (!lock_fail) {
			dev_dbg(hw_logger_data->dev, "lock fail\n");
			lock_fail = true;
		}
		return ret;
	} else if (lock_fail) {
		dev_dbg(hw_logger_data->dev, "lock success\n");
		lock_fail = false;
	}

	pwr_status = mtk_apu_get_rpc_pwr_status(hw_logger_data->power_pdev) & 0x1;
	dev_dbg(hw_logger_data->dev, "APU_RPC_PWR_STATUS = 0x%x\n", pwr_status);

	ret = get_r_w_ptr(&r_ptr, &w_ptr, hw_logger_data);

	if (ret != 0 && pwr_status != 0) {
		dev_dbg(hw_logger_data->dev, "skip copy buf, apu may power off, ret = %d\n", ret);
		goto out;
	}

	r_ofs = r_ptr - get_st_addr(hw_logger_data);
	w_ofs = w_ptr - get_st_addr(hw_logger_data);
	dev_dbg(hw_logger_data->dev, "w_ofs = 0x%x, r_ofs = 0x%x, st_addr = 0x%llx, t_size = 0x%x\n",
		w_ofs, r_ofs, get_st_addr(hw_logger_data), get_t_size());

	if (pwr_status == 0 || r_ptr == 0 || w_ptr == 0) {
		ret = regmap_field_read(hw_logger_data->log_read_regmap_field, &r_ofs);
		if (ret)
			dev_err(hw_logger_data->dev, "failed to read log r_ofs\n");

		ret = regmap_field_read(hw_logger_data->log_write_regmap_field, &w_ofs);
		if (ret)
			dev_err(hw_logger_data->dev, "failed to read log w_ofs\n");

		dev_dbg(hw_logger_data->dev, "mbox w_ofs = 0x%x, r_ofs = 0x%x\n",
			w_ofs, r_ofs);
	}

	ret = __apu_logtop_copy_buf(r_ofs, w_ofs, pwr_status, hw_logger_data);
out:
	mutex_unlock(&hw_logger_data->mutex);

	return ret;
}

static int hw_logger_copy_buf(struct mtk_apu_hw_logger *hw_logger_data)
{
	dev_dbg(hw_logger_data->dev, "in\n");

	if (!hw_logger_data->apu_logtop || !hw_logger_data->apu)
		return 0;

	return apu_logtop_copy_buf(hw_logger_data);
}

static irqreturn_t apu_logtop_irq_handler(int irq, void *dev_id)
{
	static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);

	bool lbc_full_flg, ovwrite_flg;
	unsigned int ctrl_flag = 0;
	unsigned long long irq_start_time;
	struct mtk_apu_hw_logger *hw_logger_data = dev_id;
	struct device *dev = hw_logger_data->dev;

	irq_start_time = sched_clock();

	apu_logtop_copy_buf(hw_logger_data);

	w1c32_atf(hw_logger_data->apu_logtop + APU_LOGTOP_CON_FLAG_OFF, &ctrl_flag, hw_logger_data);

	dev_dbg(dev, "w1c %s = 0x%x\n", __func__,
		(ctrl_flag & APU_LOGTOP_CON_FLAG_MASK) >> APU_LOGTOP_CON_FLAG_SHIFT);

	lbc_full_flg = !!(ctrl_flag & LBC_FULL_FLAG);
	ovwrite_flg = !!(ctrl_flag & OVWRITE_FLAG);

	if (!lbc_full_flg && !ovwrite_flg && !__ratelimit(&_rs)) {
		if (hw_logger_data->burst_intr_cnt < HW_LOG_INTR_THRESHOLD) {
			dev_dbg(dev, "apu may power off, intr status = 0x%x, intr cnt = %d\n",
				 ctrl_flag, hw_logger_data->burst_intr_cnt);
			hw_logger_data->burst_intr_cnt++;
		} else if (hw_logger_data->burst_intr_cnt == HW_LOG_INTR_THRESHOLD) {
			hw_logger_data->burst_intr_cnt++;
		}
	} else {
		hw_logger_data->burst_intr_cnt = 0;
	}

	wake_up_all(&hw_logger_data->wq_data.apusys_hwlog_wait);
	dev_dbg(dev, "irq_time = %lld ns\n", sched_clock() - irq_start_time);

	return IRQ_HANDLED;
}

static int mtk_apu_get_dbglv(void *data, u64 *val)
{
	struct mtk_apu_hw_logger *hw_logger_data = data;

	*val = hw_logger_data->hw_ipi_log_level;

	return 0;
}

static int mtk_apu_set_dbglv(void *data, u64 val)
{
	int ret;
	struct mtk_apu_hw_logger *hw_logger_data = data;
	struct mtk_apu *apu = hw_logger_data->apu;

	dev_dbg(hw_logger_data->dev, "set uP debug lv = 0x%llx\n", val);

	hw_logger_data->hw_ipi_log_level = val;

	ret = mtk_apu_power_on_off(apu->pdev, MTK_APU_IPI_LOG_LEVEL, 1, 0);
	if (ret && ret != -EOPNOTSUPP) {
		dev_err(hw_logger_data->dev, "mtk_apu_power_on_off fail(%d)\n", ret);
		return -1;
	}

	ret = mtk_apu_ipi_send(apu, MTK_APU_IPI_LOG_LEVEL,
			&hw_logger_data->hw_ipi_log_level,
			sizeof(hw_logger_data->hw_ipi_log_level),
			1000);
	if (ret)
		dev_err(hw_logger_data->dev, "Failed for hw_logger log level send.\n");

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(mtk_apu_hwlogger_dbglv_fops, mtk_apu_get_dbglv, mtk_apu_set_dbglv,
			 "%llu\n");

static int mtk_apu_hwlogger_attr_show(struct seq_file *s, void *data)
{
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	seq_printf(s, "hw_log_buf = %pK\n", hw_logger_data->hw_log_buf.hw_log_buf);

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	seq_printf(s, "__loc_log_w_ofs = %d\n", hw_logger_data->buf_track.__loc_log_w_ofs);
	seq_printf(s, "__loc_log_ov_flg = %d\n", hw_logger_data->buf_track.__loc_log_ov_flg);
	seq_printf(s, "__loc_log_sz = %d\n", hw_logger_data->buf_track.__loc_log_sz);
	seq_printf(s, "norm_log.w_ptr = %d\n", hw_logger_data->g_log.norm_mode.w_ptr);
	seq_printf(s, "norm_log.r_ptr = %d\n", hw_logger_data->g_log.norm_mode.r_ptr);
	seq_printf(s, "norm_log.ov_flg = %d\n", hw_logger_data->g_log.norm_mode.ov_flg);
	seq_printf(s, "lock_log.w_ptr = %d\n", hw_logger_data->g_log.lock_mode.w_ptr);
	seq_printf(s, "lock_log.r_ptr = %d\n", hw_logger_data->g_log.lock_mode.r_ptr);
	seq_printf(s, "lock_log.ov_flg = %d\n",	hw_logger_data->g_log.lock_mode.ov_flg);
	seq_printf(s, "lock_log.not_rd_sz = %d\n", hw_logger_data->g_log.lock_mode.not_rd_sz);
	seq_printf(s, "mobile_lock_log.w_ptr = %d\n", hw_logger_data->g_log.mobile_lock_mode.w_ptr);
	seq_printf(s, "mobile_lock_log.r_ptr = %d\n", hw_logger_data->g_log.mobile_lock_mode.r_ptr);
	seq_printf(s, "mobile_lock_log.ov_flg = %d\n", hw_logger_data->g_log.mobile_lock_mode.ov_flg);
	seq_printf(s, "mobile_lock_log.not_rd_sz = %d\n", hw_logger_data->g_log.mobile_lock_mode.not_rd_sz);

	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_apu_hwlogger_attr);

static void *mtk_apu_hw_logger_seq_start(struct seq_file *s, loff_t *pos)
{
	struct hw_log_seq_data *pSData;
	unsigned int w_ptr, r_ptr, ov_flg = 0;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	hw_logger_copy_buf(hw_logger_data);

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	if (hw_logger_data->g_log.norm_mode.r_ptr == U32_MAX) {
		w_ptr = hw_logger_data->buf_track.__loc_log_w_ofs;
		ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
		hw_logger_data->g_log.norm_mode.w_ptr = w_ptr;
		hw_logger_data->g_log.norm_mode.ov_flg = ov_flg;
		if (ov_flg)
			r_ptr = (w_ptr + HWLOG_LINE_MAX_LENS) % HWLOG_LOG_SIZE;
		else
			r_ptr = 0;
		hw_logger_data->g_log.norm_mode.r_ptr = r_ptr;
	} else {
		w_ptr = hw_logger_data->g_log.norm_mode.w_ptr;
		r_ptr = hw_logger_data->g_log.norm_mode.r_ptr;
	}
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	dev_dbg(hw_logger_data->dev, "w_ptr = %d, r_ptr = %d, ov_flg = %d, *pos = %d\n",
		w_ptr, r_ptr, ov_flg, (unsigned int)*pos);

	if (w_ptr == r_ptr) {
		if (s->size == PAGE_SIZE) {
			spin_lock_irqsave(&hw_logger_data->spinlock, flags);
			hw_logger_data->g_log.norm_mode.r_ptr = U32_MAX;
			spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);
		} else {
			s->size = PAGE_SIZE;
		}
		return NULL;
	}

	pSData = kzalloc(sizeof(struct hw_log_seq_data), GFP_KERNEL);
	if (!pSData)
		return NULL;

	pSData->w_ptr = w_ptr;
	pSData->ov_flg = ov_flg;
	if (ov_flg == 0)
		pSData->r_ptr = r_ptr;
	else
		pSData->r_ptr = w_ptr;

	return pSData;
}

static void *mtk_apu_hw_logger_seq_startl(struct seq_file *s, loff_t *pos)
{
	uint32_t w_ptr, r_ptr, ov_flg;
	struct hw_log_seq_data *pSData, *gpSData;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	pSData = kzalloc(sizeof(struct hw_log_seq_data), GFP_KERNEL);
	if (!pSData)
		return NULL;

	/* flush the last hw buffer */
	hw_logger_copy_buf(hw_logger_data);

	if (s->file && s->file->f_flags & O_NONBLOCK) {
		pSData->nonblock = true;
		gpSData = &hw_logger_data->g_log.mobile_lock_mode;
	} else {
		pSData->nonblock = false;
		gpSData = &hw_logger_data->g_log.lock_mode;
	}

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	w_ptr = hw_logger_data->buf_track.__loc_log_w_ofs;
	gpSData->w_ptr = w_ptr;
	if (gpSData->r_ptr == U32_MAX) {
		ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
		gpSData->ov_flg = ov_flg;
		if (ov_flg)
			r_ptr = (w_ptr + HWLOG_LINE_MAX_LENS) % HWLOG_LOG_SIZE;
		else
			r_ptr = 0;
		gpSData->r_ptr = r_ptr;
	} else {
		r_ptr = gpSData->r_ptr;
		/* check if overflow occurs */
		if (gpSData->not_rd_sz >= LOCAL_LOG_SIZE - HWLOG_LINE_MAX_LENS) {
			r_ptr = (w_ptr + HWLOG_LINE_MAX_LENS) % HWLOG_LOG_SIZE;
			gpSData->not_rd_sz = LOCAL_LOG_SIZE - HWLOG_LINE_MAX_LENS;
		}
	}
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	while (!signal_pending(current) && w_ptr == r_ptr) {
		if (w_ptr != r_ptr)
			break;
		else if (pSData->nonblock) {
			kfree(pSData);
			dev_dbg(hw_logger_data->dev, "END\n");
			return NULL;
		}
		usleep_range(10000, 15000);

		/* flush the last hw buffer */
		hw_logger_copy_buf(hw_logger_data);

		spin_lock_irqsave(&hw_logger_data->spinlock, flags);
		w_ptr = hw_logger_data->buf_track.__loc_log_w_ofs;
		ov_flg = hw_logger_data->buf_track.__loc_log_ov_flg;
		spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);
	};

	dev_dbg(hw_logger_data->dev, "w_ptr = %d, r_ptr = %d, ov_flg = %d, *pos = %d\n",
		w_ptr, r_ptr, ov_flg, (unsigned int)*pos);

	pSData->w_ptr = w_ptr;
	pSData->r_ptr = r_ptr;
	pSData->ov_flg = ov_flg;

	if (signal_pending(current)) {
		dev_dbg(hw_logger_data->dev, "BREAK w_ptr = %d, r_ptr = %d, ov_flg = %d\n",
			w_ptr, r_ptr, ov_flg);
		spin_lock_irqsave(&hw_logger_data->spinlock, flags);
		gpSData->r_ptr = U32_MAX;
		spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);
		kfree(pSData);
		return NULL;
	}

	return pSData;
}

static void *mtk_apu_hw_logger_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct hw_log_seq_data *pSData = v;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	pSData->r_ptr = (pSData->r_ptr + HWLOG_LINE_MAX_LENS) % LOCAL_LOG_SIZE;
	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->g_log.norm_mode.r_ptr = pSData->r_ptr;
	*pos = pSData->r_ptr + 1;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	if (pSData->r_ptr != pSData->w_ptr)
		return pSData;

	dev_dbg(hw_logger_data->dev,
		"END norm_log.w_ptr = %d, norm_log.r_ptr = %d, lock_log.ov_flg = %d\n",
		hw_logger_data->g_log.norm_mode.w_ptr, hw_logger_data->g_log.norm_mode.r_ptr,
		hw_logger_data->g_log.lock_mode.ov_flg);

	kfree(pSData);
	return NULL;
}

static void *mtk_apu_hw_logger_seq_nextl(struct seq_file *s, void *v, loff_t *pos)
{
	struct hw_log_seq_data *pSData = v, *gpSData;
	unsigned long flags;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	if (pSData->nonblock)
		gpSData = &hw_logger_data->g_log.mobile_lock_mode;
	else
		gpSData = &hw_logger_data->g_log.lock_mode;

	dev_dbg(hw_logger_data->dev,
		"w_ptr = %d, r_ptr = %d, ov_flg = %d, nonblock = %d, *pos = %d\n",
		pSData->w_ptr, pSData->r_ptr,
		pSData->ov_flg, pSData->nonblock, (unsigned int)*pos);

	pSData->r_ptr = (pSData->r_ptr + HWLOG_LINE_MAX_LENS) % LOCAL_LOG_SIZE;
	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	gpSData->r_ptr = pSData->r_ptr;
	gpSData->not_rd_sz -= HWLOG_LINE_MAX_LENS;
	*pos = pSData->r_ptr + 1;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	if (pSData->r_ptr != pSData->w_ptr)
		return pSData;

	dev_dbg(hw_logger_data->dev,
		"END gpSData w_ptr = %d, r_ptr = %d, nonblock = %d, ov_flg = %d\n",
		gpSData->w_ptr, gpSData->r_ptr, gpSData->nonblock,
		gpSData->ov_flg);

	kfree(pSData);
	return NULL;
}

static void mtk_apu_hw_logger_seq_stop(struct seq_file *s, void *v)
{
	kfree(v);
}

static int mtk_apu_hw_logger_seq_show(struct seq_file *s, void *v)
{
	struct hw_log_seq_data *pSData = v;
	static unsigned int prevIsBinary;
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	dev_dbg(hw_logger_data->dev, "(%04d)(%d) %s", pSData->r_ptr,
		(int)strlen(hw_logger_data->local_log_buf + pSData->r_ptr),
		hw_logger_data->local_log_buf + pSData->r_ptr);

	if ((hw_logger_data->local_log_buf[pSData->r_ptr] == 0xA5) &&
	    (hw_logger_data->local_log_buf[pSData->r_ptr+1] == 0xA5)) {
		prevIsBinary = 1;
		seq_write(s, hw_logger_data->local_log_buf + pSData->r_ptr, HWLOG_LINE_MAX_LENS);
	} else {
		if (prevIsBinary)
			seq_putc(s, '\n');
		prevIsBinary = 0;
		*(hw_logger_data->local_log_buf + pSData->r_ptr + HWLOG_LINE_MAX_LENS - 1) = '\0';
		seq_puts(s, hw_logger_data->local_log_buf + pSData->r_ptr);
	}

	return 0;
}

static unsigned int mtk_apu_hw_logger_seq_poll(struct file *file, poll_table *wait)
{
	unsigned int ret = 0;
	struct mtk_apu_hw_logger *hw_logger_data;

	if (!(file->f_mode & FMODE_READ))
		return ret;

	hw_logger_data = ((struct seq_file *)file->private_data)->private;

	/* flush the last hw buffer */
	hw_logger_copy_buf(hw_logger_data);

	poll_wait(file, &hw_logger_data->wq_data.apusys_hwlog_wait, wait);

	if (hw_logger_data->g_log.mobile_lock_mode.r_ptr != hw_logger_data->buf_track.__loc_log_w_ofs)
		ret = POLLIN | POLLRDNORM;

	return ret;
}

static const struct seq_operations mtk_apu_hw_logger_log_sops = {
	.start = mtk_apu_hw_logger_seq_start,
	.next  = mtk_apu_hw_logger_seq_next,
	.stop  = mtk_apu_hw_logger_seq_stop,
	.show  = mtk_apu_hw_logger_seq_show
};

static const struct seq_operations mtk_apu_hw_logger_seq_ops_locked = {
	.start = mtk_apu_hw_logger_seq_startl,
	.next  = mtk_apu_hw_logger_seq_nextl,
	.stop  = mtk_apu_hw_logger_seq_stop,
	.show  = mtk_apu_hw_logger_seq_show
};

static int debug_sqopen_lock(struct inode *inode, struct file *file)
{
	int res = seq_open(file, &mtk_apu_hw_logger_seq_ops_locked);

	if (!res)
		((struct seq_file *)file->private_data)->private = inode->i_private;
	else
		pr_err("%s: seq_open failed\n", __func__);

	return res;
}

DEFINE_SEQ_ATTRIBUTE(mtk_apu_hw_logger_log);

static const struct file_operations hw_loggerSeqLogL_ops = {
	.owner   = THIS_MODULE,
	.open    = debug_sqopen_lock,
	.poll    = mtk_apu_hw_logger_seq_poll,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static void clear_local_log_buf(struct mtk_apu_hw_logger *hw_logger_data)
{
	unsigned long flags;

	hw_logger_copy_buf(hw_logger_data);

	mutex_lock(&hw_logger_data->mutex);

	memset(hw_logger_data->local_log_buf, 0, LOCAL_LOG_SIZE);

	spin_lock_irqsave(&hw_logger_data->spinlock, flags);
	hw_logger_data->buf_track.__loc_log_w_ofs = 0;
	hw_logger_data->buf_track.__loc_log_ov_flg = 0;
	hw_logger_data->buf_track.__loc_log_sz = 0;

	hw_logger_data->g_log.norm_mode.w_ptr = 0;
	hw_logger_data->g_log.norm_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.norm_mode.ov_flg = 0;
	hw_logger_data->g_log.norm_mode.not_rd_sz = 0;
	hw_logger_data->g_log.lock_mode.w_ptr = 0;
	hw_logger_data->g_log.lock_mode.r_ptr = U32_MAX;
	hw_logger_data->g_log.lock_mode.ov_flg = 0;
	hw_logger_data->g_log.lock_mode.not_rd_sz = 0;

	hw_logger_data->g_log.mobile_lock_mode.not_rd_sz = 0;
	spin_unlock_irqrestore(&hw_logger_data->spinlock, flags);

	mutex_unlock(&hw_logger_data->mutex);
}

static int mtk_apu_hwlogger_clear_show(struct seq_file *s, void *data)
{
	struct mtk_apu_hw_logger *hw_logger_data = s->private;

	clear_local_log_buf(hw_logger_data);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_apu_hwlogger_clear);

static void hw_logger_remove_debugfs(struct mtk_apu_hw_logger *hw_logger_data)
{
	debugfs_remove_recursive(hw_logger_data->log_root);
}

static int hw_logger_create_debugfs(struct mtk_apu_hw_logger *hw_logger_data)
{
	struct dentry *log_devinfo;
	struct dentry *log_devattr;
	struct dentry *log_seqlog;
	struct dentry *log_seqlogL;
	struct dentry *log_clear;
	int ret = 0;

	hw_logger_data->log_root = debugfs_create_dir(APUSYS_HWLOGR_DIR, NULL);
	ret = IS_ERR_OR_NULL(hw_logger_data->log_root);
	if (ret) {
		dev_err(hw_logger_data->dev, "create dir fail (%d)\n", ret);
		goto out;
	}

	log_devinfo = debugfs_create_file("log_level", 0440, hw_logger_data->log_root,
					   hw_logger_data, &mtk_apu_hwlogger_dbglv_fops);
	ret = IS_ERR_OR_NULL(log_devinfo);
	if (ret) {
		dev_err(hw_logger_data->dev, "create devinfo fail (%d)\n", ret);
		goto out;
	}

	log_seqlog = debugfs_create_file("seq_log", 0440, hw_logger_data->log_root,
					  hw_logger_data, &mtk_apu_hw_logger_log_fops);
	ret = IS_ERR_OR_NULL(log_seqlog);
	if (ret) {
		dev_err(hw_logger_data->dev, "create seqlog fail (%d)\n", ret);
		goto out;
	}

	log_seqlogL = debugfs_create_file("seq_logl", 0440, hw_logger_data->log_root,
					   hw_logger_data, &hw_loggerSeqLogL_ops);
	ret = IS_ERR_OR_NULL(log_seqlogL);
	if (ret) {
		dev_err(hw_logger_data->dev, "create seqlogL fail (%d)\n", ret);
		goto out;
	}

	log_devattr = debugfs_create_file("state", 0440, hw_logger_data->log_root,
					   hw_logger_data, &mtk_apu_hwlogger_attr_fops);
	ret = IS_ERR_OR_NULL(log_devattr);
	if (ret) {
		dev_err(hw_logger_data->dev, "create attr fail (%d)\n", ret);
		goto out;
	}

	log_clear = debugfs_create_file("clear_log", 0200, hw_logger_data->log_root,
					 hw_logger_data, &mtk_apu_hwlogger_clear_fops);
	ret = IS_ERR_OR_NULL(log_clear);
	if (ret) {
		dev_err(hw_logger_data->dev, "create clear fail (%d)\n", ret);
		goto out;
	}

	return 0;

out:
	hw_logger_remove_debugfs(hw_logger_data);
	return ret;
}

static int hw_logger_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_link *link;
	struct mtk_apu_hw_logger *hw_logger_data;
	struct device_node *apu_power_np;
	struct regmap *regmap;
	int ret = 0;

	hw_logger_data = devm_kzalloc(dev, sizeof(*hw_logger_data), GFP_KERNEL);
	if (!hw_logger_data)
		return -ENOMEM;

	hw_logger_data->dev = dev;

	mutex_init(&hw_logger_data->mutex);
	spin_lock_init(&hw_logger_data->spinlock);
	spin_lock_init(&hw_logger_data->smc_spinlock);

	init_waitqueue_head(&hw_logger_data->wq_data.apusys_hwlog_wait);

	hw_logger_data->apu_logtop = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR((void const *)hw_logger_data->apu_logtop)) {
		dev_err(dev, "apu_logtop remap base fail\n");
		ret = -ENOMEM;
		return ret;
	}

	apu_power_np = of_parse_phandle(dev->of_node, "mediatek,apu-power", 0);
	if (!apu_power_np)
		return dev_err_probe(dev, -EINVAL, "failed to parse mediatek,apu-power phandle\n");

	hw_logger_data->power_pdev = of_find_device_by_node(apu_power_np);
	of_node_put(apu_power_np);
	if (!hw_logger_data->power_pdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "failed to find power device for hw logger\n");

	link = device_link_add(dev, &hw_logger_data->power_pdev->dev, DL_FLAG_PM_RUNTIME);
	if (!link)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "failed to add device link between hw logger and mtk apu power\n");

	regmap = mtk_apu_get_mbox_regmap(dev->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to get mailbox regmap\n");

	hw_logger_data->log_read_regmap_field =
		devm_regmap_field_alloc(dev, regmap, mtk_apu_log_r_reg_field);
	if (IS_ERR(hw_logger_data->log_read_regmap_field))
		return dev_err_probe(dev, PTR_ERR(hw_logger_data->log_read_regmap_field),
				     "failed to allocate log read regmap field\n");

	hw_logger_data->log_write_regmap_field =
		devm_regmap_field_alloc(dev, regmap, mtk_apu_log_w_reg_field);
	if (IS_ERR(hw_logger_data->log_write_regmap_field))
		return dev_err_probe(dev, PTR_ERR(hw_logger_data->log_write_regmap_field),
				     "failed to allocate log write regmap field\n");

	ret = hw_logger_create_debugfs(hw_logger_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create debugfs for hw logger\n");

	hw_logger_data->hw_logger_irq_number = platform_get_irq(pdev, 0);
	dev_dbg(dev, "hw_logger_irq_number = %d\n", hw_logger_data->hw_logger_irq_number);

	ret = devm_request_threaded_irq(dev, hw_logger_data->hw_logger_irq_number, NULL,
					apu_logtop_irq_handler, IRQF_ONESHOT, pdev->name,
					hw_logger_data);
	if (ret) {
		dev_err(dev, "failed to request IRQ %d\n", ret);
		goto remove_debugfs;
	}

	dev_set_drvdata(dev, hw_logger_data);

	pm_runtime_enable(dev);

	dev_dbg(dev, "%s --\n", __func__);

	return 0;

remove_debugfs:
	hw_logger_remove_debugfs(hw_logger_data);

	return ret;
}

static int hw_logger_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_apu_hw_logger *hw_logger_data = dev_get_drvdata(dev);

	pm_runtime_disable(dev);

	if (hw_logger_data->hw_logger_irq_number) {
		dev_info(dev, "disable hw logger irq\n");
		disable_irq(hw_logger_data->hw_logger_irq_number);
	}

	hw_logger_remove_debugfs(hw_logger_data);

	hw_logger_buf_free(dev, hw_logger_data);

	return 0;
}

static void hw_logger_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_apu_hw_logger *hw_logger_data = dev_get_drvdata(dev);

	dev_dbg(dev, "%s ++\n", __func__);

	if (hw_logger_data->hw_logger_irq_number) {
		dev_info(dev, "disable hw logger irq\n");
		disable_irq(hw_logger_data->hw_logger_irq_number);
	}
}

static const struct of_device_id apusys_hw_logger_of_match[] = {
	{ .compatible = "mediatek,apusys-hw-logger"},
	{},
};

static struct platform_driver hw_logger_driver = {
	.probe = hw_logger_probe,
	.remove = hw_logger_remove,
	.shutdown = hw_logger_shutdown,
	.driver = {
		.name = HWLOG_DEV_NAME,
		.of_match_table = of_match_ptr(apusys_hw_logger_of_match),
	}
};

int mtk_apu_hw_logger_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&hw_logger_driver);
	if (ret != 0) {
		pr_err("Failed to register mtk_apu hw_logger driver");
		return -ENODEV;
	}

	return ret;
}
