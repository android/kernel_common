/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef MTK_APU_CONFIG_H
#define MTK_APU_CONFIG_H

struct mtk_apu;

struct mtk_apu_ipi_config {
	u64 in_buf_da;
	u64 out_buf_da;
};

struct mtk_vpu_init_info {
	uint32_t vpu_num;
	uint32_t cfg_addr;
	uint32_t cfg_size;
	uint32_t algo_info_ptr[3 * 2];
	uint32_t rst_vec[3];
	uint32_t dmem_addr[3];
	uint32_t imem_addr[3];
	uint32_t iram_addr[3];
	uint32_t cmd_addr[3];
	uint32_t log_addr[3];
	uint32_t log_size[3];
};

struct mtk_apusys_chip_data {
	uint32_t s_code;
	uint32_t b_code;
	uint32_t r_code;
	uint32_t a_code;
};

struct mtk_apu_logger_init_info {
	uint32_t iova;
	uint32_t iova_h;
	uint32_t aov_iova;
	uint32_t aov_iova_h;
	uint32_t aov_buf_sz;
	uint32_t lbc_sz;
};

struct mtk_apummu_init_info {
	uint64_t dram[32];
	uint32_t boundary;
	uint32_t _reserved;
};

struct mtk_apu_reviser_init_info {
	uint64_t dram[32];
	uint32_t boundary;
	uint32_t _reserved;
};

struct mtk_apu_mvpu_preempt_data {
	uint32_t itcm_buffer_core_0[5];
	uint32_t l1_buffer_core_0[5];
	uint32_t itcm_buffer_core_1[5];
	uint32_t l1_buffer_core_1[5];
};

struct mtk_apu_mdla_rv_mem {
	uint64_t buf;
	uint64_t da;
	uint64_t size;
};

enum mtk_apu_user_config {
	eAPU_IPI_CONFIG = 0x0,
	eVPU_INIT_INFO,
	eAPUSYS_CHIP_DATA,
	eLOGGER_INIT_INFO,
	eREVISER_INIT_INFO,
	eMVPU_PREEMPT_DATA,
	eMDLA_MEM_INFO,
	eUSER_CONFIG_MAX
};

struct mtk_apu_config_v1_entry_table {
	u32 user_entry[eUSER_CONFIG_MAX];
	u32 _reserved;
};

struct mtk_apu_config_v1 {
	u32 header_magic;
	u32 header_rev;
	u32 entry_offset;
	u32 config_size;

	u64 time_offset;
	u64 time_diff;
	u64 time_diff_cycle;
	u32 ramdump_offset;
	u32 ramdump_type;
	u32 ramdump_module;
	u32 _reserved;

	struct mtk_apu_config_v1_entry_table entry_tbl;

	struct mtk_apu_ipi_config user0_data;
	struct mtk_vpu_init_info user1_data;
	struct mtk_apusys_chip_data user2_data;
	struct mtk_apu_logger_init_info user3_data;
	struct mtk_apu_reviser_init_info user4_data;
	struct mtk_apu_mvpu_preempt_data user5_data;
	struct mtk_apu_mdla_rv_mem user6_data;
};

void mtk_apu_ipi_config_remove(struct mtk_apu *apu);
int mtk_apu_ipi_config_init(struct mtk_apu *apu);

static inline void *get_mtk_apu_config_user_ptr(struct mtk_apu_config_v1 *conf,
						enum mtk_apu_user_config user_id)
{
	struct mtk_apu_config_v1_entry_table *entry_tbl;

	if (!conf)
		return NULL;

	if (user_id >= eUSER_CONFIG_MAX)
		return NULL;

	entry_tbl = (struct mtk_apu_config_v1_entry_table *) ((void *)conf + conf->entry_offset);

	return (void *)conf + entry_tbl->user_entry[user_id];
}
#endif /* MTK_APU_CONFIG_H */
