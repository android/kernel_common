/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef __MTK_APU_HW_LOGGER_H__
#define __MTK_APU_HW_LOGGER_H__

#include <linux/regmap.h>
#include <linux/spinlock_types.h>

struct mtk_apu;

#define APU_LOGTOP_CON_OFF			(0x0)
#define APU_LOG_BUF_ST_ADDR_OFF			(0x74)
#define APU_LOG_BUF_T_SIZE_OFF			(0x78)
#define APU_LOG_BUF_W_PTR_OFF			(0x80)
#define APU_LOG_BUF_R_PTR_OFF			(0x84)
#define APU_LOGTOP_CON_FLAG_OFF			(APU_LOGTOP_CON_OFF)
#define APU_LOGTOP_CON_FLAG_SHIFT		(8)
#define APU_LOGTOP_CON_FLAG_MASK		(0xF << APU_LOGTOP_CON_FLAG_SHIFT)
#define APU_LOGTOP_CON_ST_ADDR_HI_OFF		(APU_LOGTOP_CON_OFF)
#define APU_LOGTOP_CON_ST_ADDR_HI_SHIFT		(4)
#define APU_LOGTOP_CON_ST_ADDR_HI_MASK		(0x3 << APU_LOGTOP_CON_ST_ADDR_HI_SHIFT)

#define LBC_FULL_FLAG		BIT(0)
#define OVWRITE_FLAG		BIT(2)

#define HWLOG_LINE_MAX_LENS	(128)

#define HWLOG_LOG_SIZE		(1024 * 1024)
#define LOCAL_LOG_SIZE		(1024 * 1024)

#define HW_LOG_INTR_THRESHOLD	(10)
#define MAX_SMC_OP_NUM		(0x3)

#define LOG_W_OFS		(0x40)
#define LOG_R_OFS		(0x44)

enum {
	SMC_OP_APU_LOG_BUF_NULL = 0,
	SMC_OP_APU_LOG_BUF_T_SIZE,
	SMC_OP_APU_LOG_BUF_W_PTR,
	SMC_OP_APU_LOG_BUF_R_PTR,
	SMC_OP_APU_LOG_BUF_CON,
	SMC_OP_APU_LOG_BUF_NUM
};

struct buf_tracking {
	unsigned int __loc_log_w_ofs;
	unsigned int __loc_log_ov_flg;
	unsigned int __loc_log_sz;
	unsigned int __hw_log_r_ofs;
	unsigned int __hw_log_r_reg;
};

struct hw_log_buffer {
	char *hw_log_buf;
	dma_addr_t hw_log_buf_dma_addr;
};

struct hw_log_wq {
	struct workqueue_struct *apusys_hwlog_wq;
	struct work_struct apusys_hwlog_task;
	struct delayed_work apusys_mtklog_task;
	wait_queue_head_t apusys_hwlog_wait;
	unsigned int w_ofs;
	unsigned int r_ofs;
	unsigned int t_size;
};

struct hw_log_seq_data {
	unsigned int w_ptr;
	unsigned int r_ptr;
	unsigned int ov_flg;
	unsigned int not_rd_sz;
	bool nonblock;
};

struct global_log_data {
	struct hw_log_seq_data norm_mode;		/* for debugfs normal dump */
	struct hw_log_seq_data lock_mode;		/* for debugfs lock dump */
	struct hw_log_seq_data mobile_lock_mode;	/* for debugfs mobile logger lock dump */
};

struct mtk_apu_hw_logger {
	struct device *dev;
	void __iomem *apu_logtop;
	atomic_t apu_toplog_deep_idle;
	struct buf_tracking buf_track;
	struct global_log_data g_log;
	struct hw_log_wq wq_data;
	struct hw_log_buffer hw_log_buf;
	char *local_log_buf;
	struct mtk_apu *apu;
	struct dentry *log_root;
	unsigned int hw_ipi_log_level;
	unsigned int access_rcx_in_atf;
	unsigned int enable_interrupt;
	unsigned int hw_log_lbc_size;
	int hw_logger_irq_number;
	int burst_intr_cnt;
	int (*apu_ipi_send_ptr)(struct mtk_apu*, u32, void*, u32, u32);
	struct mutex mutex;		/* for buffer copying */
	spinlock_t spinlock;		/* for accessing hw_logger members*/
	spinlock_t smc_spinlock;	/* for executing smc operation*/
	struct regmap_field *log_read_regmap_field;
	struct regmap_field *log_write_regmap_field;
	struct platform_device *power_pdev;
};

int mtk_apu_hw_logger_ipi_init(struct mtk_apu_hw_logger *hw_logger_data, struct mtk_apu *apu);
void mtk_apu_hw_logger_ipi_remove(struct mtk_apu *apu);

dma_addr_t mtk_apu_hw_logger_config_init(struct mtk_apu_hw_logger *hw_logger_data);
struct mtk_apu_hw_logger *get_mtk_apu_hw_logger_device(struct device_node *hw_logger_np);

int mtk_apu_hw_logger_init(void);

#endif /* __MTK_APU_HW_LOGGER_H__ */
