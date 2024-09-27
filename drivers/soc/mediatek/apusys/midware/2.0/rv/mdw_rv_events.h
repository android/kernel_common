/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mdw_rv_events
#if !defined(__MDW_RV_EVENTS_H__) || defined(TRACE_HEADER_MULTI_READ)
#define __MDW_RV_EVENTS_H__
#include <linux/tracepoint.h>

#define MDW_TAG_CMD_PRINT \
	"%u,pid=%d,uid=0x%llx,rvid=0x%llx,"\
	"num_subcmds=%llu,priority=%llu,"\
	"softlimit=%u,pwr_dtime=%u,sc_rets=0x%llx,"\
	"pwr_plcy=%x,tolerance=%x,start_ts=0x%llx"\

#define MDW_TAG_SUBCMD_PRINT \
	"%u,rvid=0x%llx,inf_id=0x%llx,"\
	"%llu,vsid=%llu,ipstart_ts=0x%x,ipend_ts=0x%x,"\
	"was_preempted=0x%x,executed_core_bmp=0x%x,"\
	"tcm_usage=0x%x,history_iptime=%u,sync_info=0x%llx"\

#define MDW_TAG_CMD_DONE_PRINT \
	"%u,rvid=0x%llx,inf_id=0x%llx,enter_complt=%llu,pb_put_time=%llu"\
	"pb_put_time=%llu,handle_cmd_result_time=%llu,"\
	"load_aware_pwroff_time=%llu,enter_mpriv_release_time=%llu,"\
	"mpriv_release_time=%llu,sc_rets=0x%llx,ret=0x%llx"\

TRACE_EVENT(mdw_rv_cmd,
	TP_PROTO(uint32_t status,
		pid_t pid,
		uint64_t uid,
		uint64_t rvid,
		uint64_t num_subcmds,
		uint64_t priority,
		uint32_t softlimit,
		uint32_t pwr_dtime,
		uint64_t sc_rets,
		uint32_t pwr_plcy,
		uint32_t tolerance,
		uint64_t start_ts
		),
	TP_ARGS(status, pid, uid, rvid,
		num_subcmds, priority,
		softlimit, pwr_dtime,
		sc_rets, pwr_plcy, tolerance, start_ts
		),
	TP_STRUCT__entry(
		__field(uint32_t, status)
		__field(pid_t, pid)
		__field(uint64_t, uid)
		__field(uint64_t, rvid)
		__field(uint64_t, num_subcmds)
		__field(uint64_t, priority)
		__field(uint32_t, softlimit)
		__field(uint32_t, pwr_dtime)
		__field(uint64_t, sc_rets)
		__field(uint32_t, pwr_plcy)
		__field(uint32_t, tolerance)
		__field(uint64_t, start_ts)
	),
	TP_fast_assign(
		__entry->status = status;
		__entry->pid = pid;
		__entry->uid = uid;
		__entry->rvid = rvid;
		__entry->num_subcmds = num_subcmds;
		__entry->priority = priority;
		__entry->softlimit = softlimit;
		__entry->pwr_dtime = pwr_dtime;
		__entry->sc_rets = sc_rets;
		__entry->pwr_dtime = pwr_plcy;
		__entry->pwr_dtime = tolerance;
		__entry->pwr_dtime = start_ts;
	),
	TP_printk(
		MDW_TAG_CMD_PRINT,
		__entry->status,
		__entry->pid,
		__entry->uid,
		__entry->rvid,
		__entry->num_subcmds,
		__entry->priority,
		__entry->softlimit,
		__entry->pwr_dtime,
		__entry->sc_rets,
		__entry->pwr_plcy,
		__entry->tolerance,
		__entry->start_ts
	)
);

TRACE_EVENT(mdw_rv_subcmd,
	TP_PROTO(uint32_t status,
		uint64_t rvid,
		uint64_t inf_id,
		uint64_t sc_info,
		uint64_t vsid,
		uint32_t ipstart_ts,
		uint32_t ipend_ts,
		uint32_t was_preempted,
		uint32_t executed_core_bmp,
		uint32_t tcm_usage,
		uint32_t history_iptime,
		uint64_t sync_info
		),
	TP_ARGS(status, rvid, inf_id, sc_info, vsid,
		ipstart_ts, ipend_ts,
		was_preempted, executed_core_bmp,
		tcm_usage, history_iptime, sync_info
		),
	TP_STRUCT__entry(
		__field(uint32_t, status)
		__field(uint64_t, rvid)
		__field(uint64_t, inf_id)
		__field(uint64_t, sc_info)
		__field(uint64_t, vsid)
		__field(uint32_t, ipstart_ts)
		__field(uint32_t, ipend_ts)
		__field(uint32_t, was_preempted)
		__field(uint32_t, executed_core_bmp)
		__field(uint32_t, tcm_usage)
		__field(uint32_t, history_iptime)
		__field(uint64_t, sync_info)
	),
	TP_fast_assign(
		__entry->status = status;
		__entry->rvid = rvid;
		__entry->inf_id = inf_id;
		__entry->sc_info= sc_info;
		__entry->vsid= vsid;
		__entry->ipstart_ts = ipstart_ts;
		__entry->ipend_ts = ipend_ts;
		__entry->was_preempted = was_preempted;
		__entry->executed_core_bmp = executed_core_bmp;
		__entry->tcm_usage = tcm_usage;
		__entry->history_iptime = history_iptime;
		__entry->sync_info = sync_info;
	),
	TP_printk(
		MDW_TAG_SUBCMD_PRINT,
		__entry->status,
		__entry->rvid,
		__entry->inf_id,
		__entry->sc_info,
		__entry->vsid,
		__entry->ipstart_ts,
		__entry->ipend_ts,
		__entry->was_preempted,
		__entry->executed_core_bmp,
		__entry->tcm_usage,
		__entry->history_iptime,
		__entry->sync_info
	)
);

TRACE_EVENT(mdw_rv_cmd_deque,
	TP_PROTO(uint32_t status,
		uint64_t rvid,
		uint64_t inf_id,
		uint64_t enter_complt,
		uint64_t pb_put_time,
		uint64_t cmdbuf_out_time,
		uint64_t handle_cmd_result_time,
		uint64_t load_aware_pwroff_time,
		uint64_t enter_mpriv_release_time,
		uint64_t mpriv_release_time,
		uint64_t sc_rets,
		uint64_t c_ret
		),
	TP_ARGS(status, rvid, inf_id,
		enter_complt, pb_put_time,
		cmdbuf_out_time, handle_cmd_result_time,
		load_aware_pwroff_time, enter_mpriv_release_time, mpriv_release_time,
		sc_rets, c_ret
		),
	TP_STRUCT__entry(
		__field(uint32_t, status)
		__field(uint64_t, rvid)
		__field(uint64_t, inf_id)
		__field(uint64_t, enter_complt)
		__field(uint64_t, pb_put_time)
		__field(uint64_t, cmdbuf_out_time)
		__field(uint64_t, handle_cmd_result_time)
		__field(uint64_t, load_aware_pwroff_time)
		__field(uint64_t, enter_mpriv_release_time)
		__field(uint64_t, mpriv_release_time)
		__field(uint64_t, sc_rets)
		__field(uint64_t, c_ret)
	),
	TP_fast_assign(
		__entry->status = status;
		__entry->rvid = rvid;
		__entry->inf_id = inf_id;
		__entry->enter_complt = enter_complt;
		__entry->pb_put_time = pb_put_time;
		__entry->cmdbuf_out_time = cmdbuf_out_time;
		__entry->handle_cmd_result_time = handle_cmd_result_time;
		__entry->load_aware_pwroff_time = load_aware_pwroff_time;
		__entry->enter_mpriv_release_time = enter_mpriv_release_time;
		__entry->mpriv_release_time = mpriv_release_time;
		__entry->sc_rets = sc_rets;
		__entry->c_ret = c_ret;
	),
	TP_printk(
		MDW_TAG_CMD_DONE_PRINT,
		__entry->status,
		__entry->rvid,
		__entry->inf_id,
		__entry->enter_complt,
		__entry->pb_put_time,
		__entry->cmdbuf_out_time,
		__entry->handle_cmd_result_time,
		__entry->load_aware_pwroff_time,
		__entry->enter_mpriv_release_time,
		__entry->mpriv_release_time,
		__entry->sc_rets,
		__entry->c_ret
	)
);

#undef MDW_TAG_CMD_PRINT
#undef MDW_TAG_SUBCMD_PRINT
#undef MDW_TAG_CMD_DONE_PRINT

#endif /* #if !defined(__MDW_RV_EVENTS_H__) || defined(TRACE_HEADER_MULTI_READ) */


/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE mdw_rv_events
#include <trace/define_trace.h>

