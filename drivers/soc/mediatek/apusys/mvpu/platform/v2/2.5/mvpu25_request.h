/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __MVPU25_REQUEST_H__
#define __MVPU25_REQUEST_H__

#include "mvpu2_cmd_data.h"

/* MVPU command structure*/
struct mvpu_request_v25 {
	struct BundleHeader header;
	uint32_t algo_id;
	char name[MVPU_REQUEST_NAME_SIZE];

	/* driver info, exception etc */
	uint32_t drv_info;
	uint32_t drv_ret;

	/* mpu setting */
	uint16_t mpu_num;
	uint32_t mpu_seg[MVPU_MPU_SEGMENT_NUMS];

	/* debug mode			 */
	/* 0x0	: debugger		 */
	/* 0x1	: rv break debug */
	/* 0x2	: safe mode for memory in-order */
	uint16_t debug_mode;
	/* debugger id when rv debug */
	uint16_t debug_id;

	/* PMU setting */
	uint16_t pmu_mode;
	uint16_t pmc_mode;
	uint32_t pmu_buff;
	uint32_t buff_size;

#ifdef MVPU_SECURITY
	uint32_t batch_name_hash;

	uint32_t buf_num;
	uint32_t rp_num;

	uint64_t sec_chk_addr;
	uint64_t sec_buf_size;
	uint64_t sec_buf_attr;

	uint64_t target_buf_old_base;
	uint64_t target_buf_old_offset;
	uint64_t target_buf_new_base;
	uint64_t target_buf_new_offset;

	uint32_t kerarg_num;
	uint64_t kerarg_buf_id;
	uint64_t kerarg_offset;
	uint64_t kerarg_size;

	uint32_t primem_num;
	uint64_t primem_src_buf_id;
	uint64_t primem_dst_buf_id;
	uint64_t primem_src_offset;
	uint64_t primem_dst_offset;
	uint64_t primem_size;
#endif
	uint32_t sw_version;
	uint64_t feature_control_mask;
	uint32_t glsu_idx_mode_table;
	uint32_t glsu_idx_mode_table_size;
} __packed;

#endif /* __MVPU25_REQUEST_H__ */
