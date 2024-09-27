/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __MTK_APU_MDW_APTAG_H__
#define __MTK_APU_MDW_APTAG_H__

#include "mdw_rv.h"
#define MDW_TAGS_CNT (6000)

enum mdw_cmd_status {
	MDW_CMD_ENQUE,
	MDW_CMD_START,
	MDW_CMD_DONE,
	MDW_CMD_SCHED,
	MDW_CMD_DEQUE,
};

struct mdw_rv_tag {
	int type;

	union mdw_tag_data {
		struct mdw_tag_cmd {
			uint32_t status;
			pid_t pid;
			uint64_t param1;
			uint64_t rvid;
			uint64_t param4;
			uint64_t param5;
			uint32_t softlimit;
			uint32_t pwr_dtime;
			uint64_t param2;
			uint32_t pwr_plcy;
			uint32_t tolerance;
			uint64_t param3;
		} cmd;
		struct mdw_tag_subcmd {
			uint32_t status;
			uint64_t rvid;
			uint64_t inf_id;
			uint32_t sc_idx;
			uint32_t sc_type;
			uint64_t vsid;
			uint32_t ipstart_ts;
			uint32_t ipend_ts;
			uint32_t was_preempted;
			uint32_t executed_core_bmp;
			uint32_t tcm_usage;
			uint32_t history_iptime;
			uint64_t sync_info;
		} subcmd;
		struct mdw_tag_deque_cmd {
			uint32_t status;
			uint64_t rvid;
			uint64_t inf_id;
			uint64_t enter_complt;
			uint64_t pb_put_time;
			uint64_t cmdbuf_out_time;
			uint64_t handle_cmd_result_time;
			uint64_t load_aware_pwroff_time;
			uint64_t enter_mpriv_release_time;
			uint64_t mpriv_release_time;
			uint64_t sc_rets;
			uint64_t c_ret;
		} deqcmd;
	} d;
};

int mdw_rv_tag_init(void);
void mdw_rv_tag_deinit(void);
void mdw_rv_tag_show(struct seq_file *s);
void mdw_cmd_trace(struct mdw_cmd *c, uint32_t status);
void mdw_subcmd_trace(struct mdw_cmd *c, uint32_t sc_idx, uint64_t vsid,
		uint32_t history_iptime, uint64_t sync_info, uint32_t status);
void mdw_cmd_deque_trace(struct mdw_cmd *c, uint32_t status);

#endif
