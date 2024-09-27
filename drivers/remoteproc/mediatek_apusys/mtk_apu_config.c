// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <asm/arch_timer.h>

#include <linux/dma-mapping.h>
#include <linux/math64.h>
#include <linux/sched/clock.h>

#include <linux/mailbox/mtk-apu-mailbox.h>
#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/remoteproc/mtk_apu_hw_logger.h>
#include "mtk_apu_rproc.h"
#include "linux/soc/mediatek/apu_mvpu_config.h"

#define MTK_APU_CONFIG_HEADER_MAGIC	(0xc0de0101)
#define MTK_APU_CONFIG_HEADER_REV	(0x1)

static void mtk_apu_config_user_ptr_init(const struct mtk_apu *apu)
{
	struct mtk_apu_config_v1 *config;
	struct mtk_apu_config_v1_entry_table *entry_table;

	config = apu->conf_buf;

	config->header_magic = MTK_APU_CONFIG_HEADER_MAGIC;
	config->header_rev = MTK_APU_CONFIG_HEADER_REV;
	config->entry_offset = offsetof(struct mtk_apu_config_v1, entry_tbl);
	config->config_size = sizeof(struct mtk_apu_config_v1);

	entry_table = (struct mtk_apu_config_v1_entry_table *)((void *)config + config->entry_offset);

	entry_table->user_entry[0] = offsetof(struct mtk_apu_config_v1, user0_data);
	entry_table->user_entry[1] = offsetof(struct mtk_apu_config_v1, user1_data);
	entry_table->user_entry[2] = offsetof(struct mtk_apu_config_v1, user2_data);
	entry_table->user_entry[3] = offsetof(struct mtk_apu_config_v1, user3_data);
	entry_table->user_entry[4] = offsetof(struct mtk_apu_config_v1, user4_data);
	entry_table->user_entry[5] = offsetof(struct mtk_apu_config_v1, user5_data);
	entry_table->user_entry[6] = offsetof(struct mtk_apu_config_v1, user6_data);
}

int mtk_apu_config_setup(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	unsigned long flags;
	int ret;
	u64 timertick;
	struct mtk_apu_mdla_rv_mem *mdla_rv_mem;
	void *mdla_buf;
	dma_addr_t mdla_da;

	apu->conf_buf = dma_alloc_coherent(apu->dev, CONFIG_SIZE, &apu->conf_da, GFP_KERNEL);

	if (apu->conf_buf == NULL || apu->conf_da == 0) {
		dev_err(dev, "%s: dma_alloc_coherent fail\n", __func__);
		return -ENOMEM;
	}

	dev_dbg(dev, "%s: apu->conf_buf = %p, apu->conf_da = %pad\n", __func__,
		apu->conf_buf, &apu->conf_da);

	mtk_apu_config_user_ptr_init(apu);

	spin_lock_irqsave(&apu->reg_lock, flags);
	ret = regmap_field_write(apu->config_regmap_field, (u32)apu->conf_da);
	spin_unlock_irqrestore(&apu->reg_lock, flags);

	if (ret) {
		dev_err(dev, "Failed to set config address, ret = %d\n", ret);
		return ret;
	}

	spin_lock_irqsave(&apu->reg_lock, flags);
	apu->conf_buf->time_offset = sched_clock();
	timertick = arch_timer_read_counter();
	spin_unlock_irqrestore(&apu->reg_lock, flags);

	apu->conf_buf->time_diff = div_u64(timertick * 1000, 13) - apu->conf_buf->time_offset;
	apu->conf_buf->time_diff_cycle = timertick - div_u64(apu->conf_buf->time_offset * 13, 1000);

	ret = mtk_apu_ipi_config_init(apu);
	if (ret) {
		dev_err(dev, "apu ipi config init failed: %d\n", ret);
		goto free_apu_conf_buf;
	}

	if (apu->hw_logger_data != NULL) {
		dma_addr_t hw_log_buf_dma_addr;
		struct mtk_apu_logger_init_info *st_logger_init_info;

		hw_log_buf_dma_addr = mtk_apu_hw_logger_config_init(apu->hw_logger_data);
		st_logger_init_info = (struct mtk_apu_logger_init_info *)
			get_mtk_apu_config_user_ptr(apu->conf_buf, eLOGGER_INIT_INFO);

		if (hw_log_buf_dma_addr) {
			st_logger_init_info->iova = lower_32_bits(hw_log_buf_dma_addr);
			st_logger_init_info->iova_h = upper_32_bits(hw_log_buf_dma_addr);
			st_logger_init_info->lbc_sz = apu->hw_logger_data->hw_log_lbc_size;
			dev_dbg(dev, "set st_logger_init_info iova = 0x%x, iova_h = 0x%x, lbc_sz = 0x%x\n",
				st_logger_init_info->iova,
				st_logger_init_info->iova_h,
				st_logger_init_info->lbc_sz);
		} else {
			dev_err(dev, "hw_log_buf_dma_addr is NULL\n");
		}
	}

	ret = mvpu_config_init(apu);
	if (ret) {
		dev_info(apu->dev, "mvpu config init failed\n");
		goto free_apu_conf_buf;
	}

	mdla_rv_mem = (struct mtk_apu_mdla_rv_mem *)
		get_mtk_apu_config_user_ptr(apu->conf_buf, eMDLA_MEM_INFO);

	mdla_buf = dma_alloc_coherent(apu->dev, MDLA_RESERVE_MEM_SZ,
					      &mdla_da, GFP_KERNEL);
	if (mdla_buf == NULL || mdla_da == 0) {
		dev_err(dev, "%s() dma_alloc_coherent fail\n", __func__);
		return -ENOMEM;
	}
	mdla_rv_mem->size = MDLA_RESERVE_MEM_SZ;
	mdla_rv_mem->buf = mdla_buf;
	mdla_rv_mem->da = mdla_da;

	return 0;

free_apu_conf_buf:
	dma_free_coherent(apu->dev, CONFIG_SIZE, apu->conf_buf, apu->conf_da);

	return ret;
}

void mtk_apu_config_remove(struct mtk_apu *apu)
{
	mtk_apu_ipi_config_remove(apu);
	mvpu_config_remove(apu);

	dma_free_coherent(apu->dev, CONFIG_SIZE, apu->conf_buf, apu->conf_da);
}
