/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __APUMMU_TABLE_H__
#define __APUMMU_TABLE_H__

#define APUMMU_MAX_BUF_CNT		(16)
/* midware has max 64 SCs */
#define APUMMU_MAX_SC_CNT		(64)

#define APUMMU_MAX_TBL_ENTRY	(APUMMU_MAX_SC_CNT*APUMMU_MAX_BUF_CNT)

enum mem_mask_idx {
	DRAM_1_4G,
	DRAM_4_16G,
	SLB_EXT,
	SLB_RSV_S,
};

/* NOTE: DATA_BUF, CMD_BUF aligned MDW */
enum AMMU_BUF_TYPE {
	AMMU_DATA_BUF,	// (non 1-1 mapping)
	AMMU_CMD_BUF,	// (1-1 mapping)
	AMMU_VLM_BUF,	// (1-1 mapping)
};

struct ammu_stable_info {
	uint32_t EXT_SLB_addr;       // SLB addr for EXT

	/*
	 * Issue: there's only 5 page arrays, so we cannot use 4 page arrays for 0-16G
	 * 1 segment for 4-16G with page array size is 512M (24 fields)
	 * 1~4 4~16G
	 */
	uint32_t DRAM_page_array_mask[2];	// 1 for 0~4G, 1 for 4~16G
	uint8_t  DRAM_page_array_en_num[2];	// 0-1, for page_idx_mask[x] enable
	uint8_t  mem_mask;	// bit meaning plz refer to mem_mask_idx

	/* NOTE: No EXT_SLB_size since size is not important */
	uint8_t RSV_S_SLB_page_array_start;	// SLB PA for RSV_S
	uint8_t RSV_S_SLB_page;	// SLB size for RSV_S, shift by 512M
};

/* apummu iova-eva mapping table */
struct apummu_session_tbl {
	/* stuff pass to RV */
	struct ammu_stable_info stable_info;
	uint64_t session;

	uint32_t DRAM_1_4G_mask_cnter[32];
	uint32_t DRAM_4_16G_mask_cnter[32];
	uint32_t subcmd_num_for_DRAM_FB;

	struct list_head list;
};

struct apummu_tbl {
	struct list_head g_stable_head;
	struct kref session_tbl_cnt;
	struct mutex table_lock;
	struct mutex DRAM_FB_lock;
	uint16_t subcmd_refcnt;
	uint8_t alloc_subcmd_refcnt;
	bool is_VLM_info_IPI_sent; // to set VLM DRAM FB or clean setting
	bool is_SLB_set;
	bool is_work_canceled;
	bool is_free_job_set;
	bool is_SLB_alloc; // Since SLB state might not sync with APU
};

int ammu_DRAM_FB_alloc(uint64_t session, uint32_t vlm_size, uint32_t subcmd_num);
int addr_encode_and_write_stable(enum AMMU_BUF_TYPE type, uint64_t session,
			uint64_t iova, uint64_t buf_size, uint64_t *eva);
int apummu_eva_decode(uint64_t eva, uint64_t *iova, enum AMMU_BUF_TYPE type);
int apummu_stable_buffer_remove(uint64_t session, uint64_t device_va,
			uint32_t buf_size);
int get_session_table(uint64_t session, void **tbl_kva, uint32_t *size);
int session_table_free(uint64_t session);
void dump_session_table_set(void);
void apummu_mgt_init(void);
void apummu_mgt_destroy(void);

int ammu_session_table_add_SLB(uint64_t session, uint32_t type);
int ammu_session_table_remove_SLB(uint64_t session, uint32_t type);
void ammu_session_table_check_SLB(uint32_t type);

#endif //Endof __APUMMU_TABLE_H__
