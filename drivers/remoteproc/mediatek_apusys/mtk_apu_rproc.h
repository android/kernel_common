/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef MTK_APU_RPROC_H
#define MTK_APU_RPROC_H

#define MBOX_HOST_CONFIG_ADDR (0x48)

#define CONFIG_SIZE		(round_up(sizeof(struct mtk_apu_config_v1), PAGE_SIZE))
#define DRAM_OFFSET		(0x00000UL)
#define TCM_OFFSET		(0x4d000000UL)
#define CODE_BUF_DA		(DRAM_OFFSET)
#define MTK_APU_SEC_FW_IOVA	(0x200000UL)
#define MDLA_RESERVE_MEM_SZ	(0x10000)

#define MIN_DTIME		(0)
#define MAX_DTIME		(10000)

int mtk_apu_exception_init(struct platform_device *pdev, struct mtk_apu *apu);
void mtk_apu_exception_exit(struct platform_device *pdev, struct mtk_apu *apu);
void mtk_apu_init_mdw_dtime_setting(void (*update_power_dtime)(int dtime));
uint32_t mtk_apu_rv_smc_call(struct device *dev, uint32_t smc_id, uint32_t a2);
struct regmap *mtk_apu_get_mbox_regmap(struct device_node *node);
void mtk_apu_init_mdw_dev(struct device *dev);

#endif /* MTK_APU_RPROC_H */
