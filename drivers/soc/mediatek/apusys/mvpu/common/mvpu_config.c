// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <asm/delay.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>

#include "linux/remoteproc/mtk_apu.h"
#include "linux/remoteproc/mtk_apu_config.h"

#include "mvpu_plat.h"
#include "linux/soc/mediatek/apu_mvpu_config.h"

#define PLAT_POLL_TIMEOUT	(1000*100)	// 1000 ms
#define PLAT_POLL_PERIOD	(10)	// 10 us

int mvpu_config_init(struct mtk_apu *apu)
{
	uint32_t id, level = 0;
	uint32_t *addr0, *addr1;
	dma_addr_t mvpu_da_l1;
	dma_addr_t mvpu_da_itcm;
	struct mtk_apu_mvpu_preempt_data *info;
	struct mvpu_preempt_buffer *preempt_buffer;
	uint32_t nr_core_ids;
	uint32_t sw_preemption_level;

	while (g_mvpu_platdata == NULL)
		for (int i=0; i<PLAT_POLL_TIMEOUT; i++)
			udelay(PLAT_POLL_PERIOD);

	if (g_mvpu_platdata == NULL) {
		pr_err("%s: g_mvpu_platdata is NULL\n", __func__);
		return -EINVAL;
	}

	preempt_buffer = &g_mvpu_platdata->preempt_buffer;
	nr_core_ids = g_mvpu_platdata->core_num;
	sw_preemption_level = g_mvpu_platdata->sw_preemption_level;

	pr_info("%s core number = %d, sw_preemption_level = 0x%x\n", __func__,
			nr_core_ids, sw_preemption_level);

	info = (struct mtk_apu_mvpu_preempt_data *) get_mtk_apu_config_user_ptr(
		apu->conf_buf, eMVPU_PREEMPT_DATA);

	for (id = 0; id < nr_core_ids; id++) {

		for (level = 0; level < sw_preemption_level; level++) {
			if (id == 0) {
				addr0 = dma_alloc_coherent(apu->dev, PREEMPT_ITCM_BUFFER,
					&mvpu_da_itcm, GFP_KERNEL);
				preempt_buffer->itcm_kernel_addr_core_0[level] = addr0;

				info->itcm_buffer_core_0[level] = (uint32_t) mvpu_da_itcm;

				if (addr0 == NULL || mvpu_da_itcm == 0) {
					pr_info("%s: dma_alloc_coherent fail\n", __func__);
					return -ENOMEM;
				}

				pr_info("core 0 itcm kernel va = 0x%lx, core 0 itcm iova = %pad\n",
					(unsigned long)addr0, &mvpu_da_itcm);

				memset(addr0, 0, PREEMPT_ITCM_BUFFER);

				addr1 = dma_alloc_coherent(apu->dev, PREEMPT_L1_BUFFER,
					&mvpu_da_l1, GFP_KERNEL);
				preempt_buffer->l1_kernel_addr_core_0[level] = addr1;
				info->l1_buffer_core_0[level] = mvpu_da_l1;

				if (addr1 == NULL || mvpu_da_l1 == 0) {
					pr_info("dma_alloc_coherent fail\n");

					dma_free_coherent(apu->dev, PREEMPT_ITCM_BUFFER,
							addr0, mvpu_da_itcm);

					return -ENOMEM;
				}

				pr_info("core 0 L1 kernel va = 0x%lx, core 0 L1 iova = %pad\n",
					(unsigned long)addr1, &mvpu_da_l1);

				memset(addr1, 0, PREEMPT_L1_BUFFER);

			} else if (id == 1) {

				addr0 = dma_alloc_coherent(apu->dev, PREEMPT_ITCM_BUFFER,
					&mvpu_da_itcm, GFP_KERNEL);
				preempt_buffer->itcm_kernel_addr_core_1[level] = addr0;

				info->itcm_buffer_core_1[level] = (uint32_t) mvpu_da_itcm;

				if (addr0 == NULL || mvpu_da_itcm == 0) {
					pr_info("dma_alloc_coherent fail\n");
					return -ENOMEM;
				}

				pr_info("addr0 = 0x%lx, mvpu_da_itcm = %pad\n",
					(unsigned long)addr0, &mvpu_da_itcm);

				memset(addr0, 0, PREEMPT_ITCM_BUFFER);

				addr1 = dma_alloc_coherent(apu->dev, PREEMPT_L1_BUFFER,
					&mvpu_da_l1, GFP_KERNEL);

				preempt_buffer->l1_kernel_addr_core_1[level] = addr1;
				info->l1_buffer_core_1[level] = mvpu_da_l1;

				if (addr1 == NULL || mvpu_da_l1 == 0) {
					pr_info("dma_alloc_coherent fail\n");
					dma_free_coherent(apu->dev, PREEMPT_ITCM_BUFFER,
						addr0, mvpu_da_itcm);
					return -ENOMEM;
				}

				pr_info("addr0 = 0x%lx, mvpu_da_itcm = %pad\n",
					(unsigned long)addr1, &mvpu_da_l1);

				memset(addr1, 0, PREEMPT_L1_BUFFER);

			} else {
				pr_info("nr_core_ids error\n");
			}
		}
	}
	return 0;
}
EXPORT_SYMBOL_NS(mvpu_config_init, MTK_APU_MVPU);

int mvpu_config_remove(struct mtk_apu *apu)
{
	int id, level = 0;
	struct mtk_apu_mvpu_preempt_data *info;
	struct mvpu_preempt_buffer *preempt_buffer = &g_mvpu_platdata->preempt_buffer;
	uint32_t nr_core_ids = g_mvpu_platdata->core_num;
	uint32_t sw_preemption_level = g_mvpu_platdata->sw_preemption_level;

	info = (struct mtk_apu_mvpu_preempt_data *) get_mtk_apu_config_user_ptr(
		apu->conf_buf, eMVPU_PREEMPT_DATA);

	for (id = 0; id < nr_core_ids; id++) {
		for (level = 0; level < sw_preemption_level; level++) {

			if (id == 0) {

				if (!preempt_buffer->l1_kernel_addr_core_0[level] ||
					!info->l1_buffer_core_0[level]) {
					dma_free_coherent(apu->dev, PREEMPT_L1_BUFFER,
					preempt_buffer->l1_kernel_addr_core_0[level],
					info->l1_buffer_core_0[level]);
				}

				if (!preempt_buffer->itcm_kernel_addr_core_0[level] ||
					!info->itcm_buffer_core_0[level]) {
					dma_free_coherent(apu->dev, PREEMPT_ITCM_BUFFER,
					preempt_buffer->itcm_kernel_addr_core_0[level],
					info->itcm_buffer_core_0[level]);
				}

			} else {
				if (!preempt_buffer->l1_kernel_addr_core_1[level] ||
					!info->l1_buffer_core_1[level]) {
					dma_free_coherent(apu->dev, PREEMPT_L1_BUFFER,
					preempt_buffer->l1_kernel_addr_core_1[level],
					info->l1_buffer_core_1[level]);
				}

				if (!preempt_buffer->itcm_kernel_addr_core_1[level] ||
					!info->itcm_buffer_core_1[level]) {
					dma_free_coherent(apu->dev, PREEMPT_ITCM_BUFFER,
					preempt_buffer->itcm_kernel_addr_core_1[level],
					info->itcm_buffer_core_1[level]);
				}
			}

		}
	}
	return 0;
}
EXPORT_SYMBOL_NS(mvpu_config_remove, MTK_APU_MVPU);
