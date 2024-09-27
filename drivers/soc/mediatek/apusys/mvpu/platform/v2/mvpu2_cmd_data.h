/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __MVPU2_CMD_DATA_H__
#define __MVPU2_CMD_DATA_H__

#define MVPU_PE_NUM  64
#define MVPU_DUP_BUF_SIZE  (2 * MVPU_PE_NUM)

#define MVPU_REQUEST_NAME_SIZE 32
#define MVPU_MPU_SEGMENT_NUMS  39

#define MVPU_MIN_CMDBUF_NUM     2
#define MVPU_CMD_INFO_IDX       0
#define MVPU_CMD_KREG_BASE_IDX  1

#define MVPU_CMD_LITE_SIZE_0 0x12E
#define MVPU_CMD_LITE_SIZE_1 0x14A

#ifndef MVPU_SECURITY
#define MVPU_SECURITY
#endif

#ifdef MVPU_SECURITY
#define BUF_NUM_MASK     0x0000FFFF

#define KERARG_NUM_MASK  0x3FFF0000
#define KERARG_NUM_SHIFT 16

#define SEC_LEVEL_MASK   0xC0000000
#define SEC_LEVEL_SHIFT  30

enum MVPU_SEC_LEVEL {
	SEC_LVL_CHECK = 0,
	SEC_LVL_CHECK_ALL = 1,
	SEC_LVL_PROTECT = 2,
	SEC_LVL_END,
};
#endif

#define MVPU_REQ_FEATURE_HSE_ENABLE     (1ULL << 0)
#define MVPU_REQ_FEATURE_OSGB_LIMITED   (1ULL << 1)

struct BundleHeader {
	union {
		unsigned int dwValue;
		struct {
			unsigned int kreg_bundle_thread_mode_cfg : 8;
			unsigned int kreg_bundle_high_priority : 1;
			unsigned int kreg_bundle_interrupt_en : 1;
			unsigned int kreg_0x0000_rsv0	  : 2;
			unsigned int kreg_kernel_thread_mode : 2;
			unsigned int kreg_0x0000_rsv1	  : 2;
			unsigned int kreg_kernel_num	  : 16;
		} s;
	} reg_bundle_setting_0;

	union {
		unsigned int dwValue;
		struct {
			unsigned int kreg_bundle_cfg_base : 32;
		} s;
	} reg_bundle_setting_1;

	union {
		unsigned int dwValue;
		struct {
			unsigned int kreg_bundle_event_id : 32;
		} s;
	} reg_bundle_setting_2;

	union {
		unsigned int dwValue;
		struct {
			unsigned int kreg_kernel_start_cnt : 16;
			unsigned int kreg_bundle_skip_dma_num : 4;
		} s;
	} reg_bundle_setting_3;
};

#endif /* __MVPU2_CMD_DATA_H__ */
