/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef MTK_APU_H
#define MTK_APU_H
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/mailbox_client.h>
#include <linux/regmap.h>

#include <linux/remoteproc/mtk_apu_config.h>
#include <linux/remoteproc/mtk_apu_ipi.h>

struct mtk_apu;

struct mtk_apu_hw_ops {
	int (*init)(struct mtk_apu *apu);
	int (*exit)(struct mtk_apu *apu);
	int (*start)(struct mtk_apu *apu);
	int (*setup)(struct mtk_apu *apu);
	int (*stop)(struct mtk_apu *apu);
	int (*mtk_apu_memmap_init)(struct mtk_apu *apu);
	void (*mtk_apu_memmap_remove)(struct mtk_apu *apu);
	int (*power_on_off)(struct mtk_apu *apu, u32 id, u32 on, u32 off);

	int (*suspend)(struct mtk_apu *apu);
	int (*resume)(struct mtk_apu *apu);
};

struct mtk_apu_plat_config {
	uint64_t up_code_buf_sz;
	uint64_t up_coredump_buf_sz;
	uint64_t regdump_buf_sz;
	uint64_t mdla_coredump_buf_sz;
	uint64_t mvpu_coredump_buf_sz;
	uint64_t mvpu_sec_coredump_buf_sz;
	uint64_t up_tcm_sz;
	uint64_t ce_coredump_buf_sz;
};

struct mtk_apu_flag {
	bool auto_boot;
	bool bypass_iommu;
	bool debug_log_on;
	bool fast_on_off;
	bool infra_wa;
	bool kernel_load_image;
	bool map_iova;
	bool preload_firmware;
	bool secure_boot;
	bool secure_coredump;
	bool smmu_support;
};

struct mtk_apu_platdata {
	struct mtk_apu_flag flags;
	struct mtk_apu_plat_config config;
	struct mtk_apu_hw_ops ops;
	char *fw_name;
};

struct apusys_secure_info_t {
	unsigned int total_sz;
	unsigned int up_code_buf_ofs;
	unsigned int up_code_buf_sz;

	unsigned int up_fw_ofs;
	unsigned int up_fw_sz;
	unsigned int up_xfile_ofs;
	unsigned int up_xfile_sz;
	unsigned int mdla_fw_boot_ofs;
	unsigned int mdla_fw_boot_sz;
	unsigned int mdla_fw_main_ofs;
	unsigned int mdla_fw_main_sz;
	unsigned int mdla_xfile_ofs;
	unsigned int mdla_xfile_sz;
	unsigned int mvpu_fw_ofs;
	unsigned int mvpu_fw_sz;
	unsigned int mvpu_xfile_ofs;
	unsigned int mvpu_xfile_sz;
	unsigned int mvpu_sec_fw_ofs;
	unsigned int mvpu_sec_fw_sz;
	unsigned int mvpu_sec_xfile_ofs;
	unsigned int mvpu_sec_xfile_sz;

	unsigned int up_coredump_ofs;
	unsigned int up_coredump_sz;
	unsigned int mdla_coredump_ofs;
	unsigned int mdla_coredump_sz;
	unsigned int mvpu_coredump_ofs;
	unsigned int mvpu_coredump_sz;
	unsigned int mvpu_sec_coredump_ofs;
	unsigned int mvpu_sec_coredump_sz;

	unsigned int ce_bin_ofs;
	unsigned int ce_bin_sz;
};

struct mtk_apu {
	struct rproc *rproc;
	struct platform_device *pdev;
	struct device *dev;
	void *md32_tcm;
	void *apu_rpc;
	void *apu_infra_hwsem;
	void *apu_sec_mem_base;
	int wdt_irq_number;
	spinlock_t reg_lock;

	struct apusys_secure_info_t *apusys_sec_info;
	uint64_t apusys_sec_mem_start;
	uint64_t apusys_sec_mem_size;

	/* Buffer to place execution area */
	void *code_buf;
	dma_addr_t code_da;

	/* hw_logger */
	struct mtk_apu_hw_logger *hw_logger_data;

	/* mailbox */
	struct mbox_client cl;
	struct mbox_chan *ch;

	/* Buffer to place config area */
	struct mtk_apu_config_v1 *conf_buf;
	dma_addr_t conf_da;

	/* to synchronize boot status of remote processor */
	struct mtk_apu_run run;

	/* to prevent multiple ipi_send run concurrently */
	struct mutex send_lock;
	struct mutex power_lock;
	struct mutex forbid_ipi_lock;
	spinlock_t usage_cnt_lock;
	struct mtk_apu_ipi_desc ipi_desc[MTK_APU_IPI_MAX];
	u32 ipi_id;
	bool ipi_id_ack[MTK_APU_IPI_MAX]; /* per-ipi ack */
	bool ipi_inbound_locked;
	bool bypass_pwr_off_chk;
	wait_queue_head_t ack_wq; /* for waiting for ipi ack */
	bool forbid_ipi_send; /* Forbid ipi sending request when system want to suspend */

	/* ipi share buffer */
	dma_addr_t recv_buf_da;
	struct mtk_share_obj *recv_buf;
	dma_addr_t send_buf_da;
	struct mtk_share_obj *send_buf;
	struct mtk_ipi_task ipi_task;

	struct rproc_subdev *rpmsg_subdev;

	const struct mtk_apu_platdata *platdata;

	/* timesync workqueue */
	struct work_struct timesync_work;
	struct workqueue_struct *timesync_workq;

	uint32_t up_code_buf_sz;
	uint32_t local_pwr_ref_cnt;
	uint32_t ipi_pwr_ref_cnt[MTK_APU_IPI_MAX];

	/* reserved memory */
	uint64_t resv_mem_pa;
	void *resv_mem_va;

	/* delay power off */
	struct timer_list power_off_timer;
	uint64_t cur_dtime_ts;

	struct regmap_field *config_regmap_field;

	const struct firmware *fw;
	struct platform_device *power_pdev;

	bool is_under_lp_scp_recovery_flow;

	unsigned int tx_serial_no;
	unsigned int rx_serial_no;
	unsigned int temp_buf[MTK_APU_SHARE_BUFFER_SIZE / 4];
	int current_ipi_handler_id;
};

int mtk_apu_config_setup(struct mtk_apu *apu);
void mtk_apu_config_remove(struct mtk_apu *apu);
int mtk_apu_ipi_init(struct platform_device *pdev, struct mtk_apu *apu);
void mtk_apu_ipi_remove(struct mtk_apu *apu);
int mtk_apu_ipi_register(struct mtk_apu *apu, u32 id, ipi_top_handler_t top_handler,
			 ipi_handler_t handler, void *priv);
void mtk_apu_ipi_unregister(struct mtk_apu *apu, u32 id);
int mtk_apu_ipi_send(struct mtk_apu *apu, u32 id, void *data, u32 len, u32 wait_ms);
int mtk_apu_timesync_init(struct mtk_apu *apu);
void mtk_apu_timesync_remove(struct mtk_apu *apu);
int mtk_apu_mem_init(struct mtk_apu *apu);

int mtk_apu_load(struct rproc *rproc, const struct firmware *fw);

#endif /* MTK_APU_H */
