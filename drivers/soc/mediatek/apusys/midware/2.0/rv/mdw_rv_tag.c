// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/tracepoint.h>

#include "mdw_rv.h"
#include "mdw_rv_tag.h"
#include "mdw_rv_events.h"
#include "mdw_cmn.h"

/* 64-bit Subcmd information : [subcmd type(32bit) | subcmd index(32bit)] */
#define MDW_SUBCMD_GEN_INFO(sc_type, sc_idx) ((sc_type << 32) | sc_idx)

#define MDW_SUBCMD_GEN_INDEX(sc_info) (sc_info & 0xffffffff)
#define MDW_SUBCMD_GEN_TYPE(sc_info) (sc_info >> 32)

enum mdw_tag_type {
	MDW_TAG_CMD,
	MDW_TAG_SUBCMD,
	MDW_TAG_CMD_DEQUE,
};

#if IS_ENABLED(CONFIG_MTK_APUSYS_DEBUG)

static struct apu_tags *mdw_rv_tags;

void mdw_cmd_trace(struct mdw_cmd *c, uint32_t status)
{
	uint64_t param1 = 0, param2 = 0, param3 = 0, param4 = 0, param5 = 0;

	if (status == MDW_CMD_ENQUE) {
		param1 = c->tgid;
		param2 = c->power_etime;
		param3 = c->inference_id;
		param4 = c->num_subcmds;
		param5 = c->priority;
	} else if (status == MDW_CMD_START) {
		param1 = c->uid;
		param2 = c->inference_id;
		param3 = c->start_ts;
		param4 = c->num_subcmds;
		param5 = c->priority;
	} else if (status == MDW_CMD_DONE) {
		param1 = c->rv_cb_time;
		param2 = c->inference_id;
		param3 = c->enter_rv_cb_time;
	}

	trace_mdw_rv_cmd(status,
		c->pid,
		param1,
		c->rvid,
		param4,
		param5,
		c->softlimit,
		c->power_dtime,
		param2,
		c->power_plcy,
		c->tolerance_ms,
		param3);
}

/* The parameters must aligned with trace_mdw_rv_cmd() */
static void
probe_rv_mdw_cmd(void *data, uint32_t status, pid_t pid,
		uint64_t param1, uint64_t rvid,
		uint64_t param4,
		uint64_t param5,
		uint32_t softlimit,
		uint32_t pwr_dtime,
		uint64_t param2,
		uint32_t pwr_plcy,
		uint32_t tolerance,
		uint64_t param3)
{
	struct mdw_rv_tag t;

	if (!mdw_rv_tags)
		return;

	t.type = MDW_TAG_CMD;
	t.d.cmd.status = status;
	t.d.cmd.pid = pid;
	t.d.cmd.param1 = param1;
	t.d.cmd.rvid = rvid;
	t.d.cmd.param4 = param4;
	t.d.cmd.param5 = param5;
	t.d.cmd.softlimit = softlimit;
	t.d.cmd.pwr_dtime = pwr_dtime;
	t.d.cmd.param2 = param2;
	t.d.cmd.pwr_plcy = pwr_plcy;
	t.d.cmd.tolerance = tolerance;
	t.d.cmd.param3 = param3;

	apu_tag_add(mdw_rv_tags, &t);
}

void mdw_subcmd_trace(struct mdw_cmd *c, uint32_t sc_idx, uint64_t vsid,
		uint32_t history_iptime, uint64_t sync_info, uint32_t status)
{

	struct mdw_subcmd_exec_info *sc_einfo = NULL;
	uint64_t sc_type = 0, sc_info = 0;

	sc_einfo = &c->einfos->sc;
	sc_type = c->subcmds[sc_idx].type;
	sc_info = MDW_SUBCMD_GEN_INFO(sc_type, sc_idx);

	trace_mdw_rv_subcmd(status,
		c->rvid, c->inference_id,
		sc_info,
		vsid,
		sc_einfo[sc_idx].ip_start_ts,
		sc_einfo[sc_idx].ip_end_ts,
		sc_einfo[sc_idx].was_preempted,
		sc_einfo[sc_idx].executed_core_bitmap,
		sc_einfo[sc_idx].tcm_usage,
		history_iptime,
		sync_info);
}

/* The parameters must aligned with trace_mdw_rv_subcmd() */
static void
probe_rv_mdw_subcmd(void *data, uint32_t status,
		uint64_t rvid, uint64_t inf_id,
		uint64_t sc_info,
		uint64_t vsid,
		uint32_t ipstart_ts,
		uint32_t ipend_ts,
		uint32_t was_preempted,
		uint32_t executed_core_bmp,
		uint32_t tcm_usage,
		uint32_t history_iptime,
		uint64_t sync_info)
{
	struct mdw_rv_tag t;

	if (!mdw_rv_tags)
		return;

	t.type = MDW_TAG_SUBCMD;
	t.d.subcmd.status = status;
	t.d.subcmd.rvid = rvid;
	t.d.subcmd.inf_id = inf_id;
	t.d.subcmd.sc_idx = MDW_SUBCMD_GEN_INDEX(sc_info);
	t.d.subcmd.sc_type = MDW_SUBCMD_GEN_TYPE(sc_info);
	t.d.subcmd.vsid = vsid;
	t.d.subcmd.ipstart_ts = ipstart_ts;
	t.d.subcmd.ipend_ts = ipend_ts;
	t.d.subcmd.was_preempted = was_preempted;
	t.d.subcmd.executed_core_bmp = executed_core_bmp;
	t.d.subcmd.tcm_usage = tcm_usage;
	t.d.subcmd.history_iptime = history_iptime;
	t.d.subcmd.sync_info = sync_info;

	apu_tag_add(mdw_rv_tags, &t);
}

void mdw_cmd_deque_trace(struct mdw_cmd *c, uint32_t status)
{
	trace_mdw_rv_cmd_deque(status,
		c->rvid,
		c->inference_id,
		c->enter_complt_time,
		c->pb_put_time,
		c->cmdbuf_out_time,
		c->handle_cmd_result_time,
		c->load_aware_pwroff_time,
		c->enter_mpriv_release_time,
		c->mpriv_release_time,
		c->einfos->c.sc_rets,
		c->einfos->c.ret);
}

/* The parameters must aligned with trace_mdw_rv_cmd_deque() */
static void
probe_rv_mdw_cmd_deque(void *data, uint32_t status,
		uint64_t rvid, uint64_t inf_id, uint64_t enter_complt,
		uint64_t pb_put_time, uint64_t cmdbuf_out_time,
		uint64_t handle_cmd_result_time,
		uint64_t load_aware_pwroff_time,
		uint64_t enter_mpriv_release_time,
		uint64_t mpriv_release_time,
		uint64_t sc_rets,
		uint64_t c_ret)

{
	struct mdw_rv_tag t;

	if (!mdw_rv_tags)
		return;

	t.type = MDW_TAG_CMD_DEQUE;
	t.d.deqcmd.status = status;
	t.d.deqcmd.rvid = rvid;
	t.d.deqcmd.inf_id = inf_id;
	t.d.deqcmd.enter_complt= enter_complt;
	t.d.deqcmd.pb_put_time = pb_put_time;
	t.d.deqcmd.cmdbuf_out_time = cmdbuf_out_time;
	t.d.deqcmd.handle_cmd_result_time = handle_cmd_result_time;
	t.d.deqcmd.load_aware_pwroff_time = load_aware_pwroff_time;
	t.d.deqcmd.enter_mpriv_release_time = enter_mpriv_release_time;
	t.d.deqcmd.mpriv_release_time = mpriv_release_time;
	t.d.deqcmd.sc_rets = sc_rets;
	t.d.deqcmd.c_ret = c_ret;

	apu_tag_add(mdw_rv_tags, &t);
}

static void mdw_rv_tag_enq_printf(struct seq_file *s, struct mdw_rv_tag *t)
{
	char status[8] = "";

	if (snprintf(status, sizeof(status)-1, "%s", "enque") < 0)
		return;

	seq_printf(s, "%s,", status);
	seq_printf(s, "pid=%d,inf_id=0x%llx,tgid=%llu,rvid=0x%llx,",
		t->d.cmd.pid, t->d.cmd.param3, t->d.cmd.param1, t->d.cmd.rvid);
	seq_printf(s, "num_subcmds=%llu,", t->d.cmd.param4);
	seq_printf(s, "priority=%llu,softlimit=%u,",
		t->d.cmd.param5, t->d.cmd.softlimit);
	seq_printf(s, "pwr_dtime=%u,", t->d.cmd.pwr_dtime);
	seq_printf(s, "pwr_etime=%llu,", t->d.cmd.param2);
	seq_printf(s, "pwr_plcy=0x%x,tolerance=0x%x\n",
		t->d.cmd.pwr_plcy, t->d.cmd.tolerance);

}

static void mdw_rv_tag_start_printf(struct seq_file *s, struct mdw_rv_tag *t)
{
	char status[8] = "";

	if (snprintf(status, sizeof(status)-1, "%s", "start") < 0)
		return;

	seq_printf(s, "%s,", status);
	seq_printf(s, "pid=%d,inf_id=0x%llx,uid=0x%llx,rvid=0x%llx,",
		t->d.cmd.pid, t->d.cmd.param2, t->d.cmd.param1, t->d.cmd.rvid);
		seq_printf(s, "start_ts=0x%llx\n", t->d.cmd.param3);

}

static void mdw_rv_tag_done_printf(struct seq_file *s, struct mdw_rv_tag *t)
{
	char status[8] = "";

	if (snprintf(status, sizeof(status)-1, "%s", "done") < 0)
		return;

	seq_printf(s, "%s,", status);
	seq_printf(s, "pid=%d,inf_id=0x%llx,rvid=0x%llx,",
		t->d.cmd.pid, t->d.cmd.param2, t->d.cmd.rvid);
	seq_printf(s, "enter_rv_cb=%llu,", t->d.cmd.param3);
	seq_printf(s, "rv_cb=%llu\n", t->d.cmd.param1);
}

static void mdw_rv_tag_seq_cmd(struct seq_file *s, struct mdw_rv_tag *t)
{
	if (t->d.cmd.status == MDW_CMD_DONE) {
		mdw_rv_tag_done_printf(s, t);
		return;
	} else if (t->d.cmd.status == MDW_CMD_START) {
		mdw_rv_tag_start_printf(s, t);
		return;
	} else if (t->d.cmd.status == MDW_CMD_ENQUE) {
		mdw_rv_tag_enq_printf(s, t);
		return;
	}
}

static void mdw_rv_tag_seq_subcmd(struct seq_file *s, struct mdw_rv_tag *t)
{
	char status[8] = "";
	char sc_idx[8] = "";

	if (snprintf(status, sizeof(status)-1, "%s", "sched") < 0)
		return;

	if (snprintf(sc_idx, sizeof(sc_idx)-1, "sc#%d:", t->d.subcmd.sc_idx) < 0)
		return;

	seq_printf(s, "%s,", status);
	seq_printf(s, "inf_id=0x%llx,rvid=0x%llx,%s",
		t->d.subcmd.inf_id, t->d.subcmd.rvid, sc_idx);
	seq_printf(s, "type=%u,vsid=0x%llx,ipstart_ts=0x%x,ipend_ts=0x%x,",
		t->d.subcmd.sc_type , t->d.subcmd.vsid, t->d.subcmd.ipstart_ts,
		t->d.subcmd.ipend_ts);
	seq_printf(s, "was_preempted=0x%x,exc_core_bmp=0x%x,",
		t->d.subcmd.was_preempted,
		t->d.subcmd.executed_core_bmp);
	seq_printf(s, "tcm_usage=0x%x,h_iptime=%u,sync_info=0x%llx\n",
		t->d.subcmd.tcm_usage, t->d.subcmd.history_iptime, t->d.subcmd.sync_info);
}

static void mdw_rv_tag_seq_cmd_deque(struct seq_file *s, struct mdw_rv_tag *t)
{
	char status[8] = "";

	if (snprintf(status, sizeof(status)-1, "%s", "deque") < 0)
		return;

	seq_printf(s, "%s,", status);
	seq_printf(s, "inf_id=0x%llx,", t->d.deqcmd.inf_id);
	seq_printf(s, "rvid=0x%llx,", t->d.deqcmd.rvid);
	seq_printf(s, "enter_complt=%llu,", t->d.deqcmd.enter_complt);
	seq_printf(s, "pb_put=%llu,", t->d.deqcmd.pb_put_time);
	seq_printf(s, "cb_out=%llu,", t->d.deqcmd.cmdbuf_out_time);
	seq_printf(s, "hnd_cmd=%llu,", t->d.deqcmd.handle_cmd_result_time);
	seq_printf(s, "las_pwroff=%llu,", t->d.deqcmd.load_aware_pwroff_time);
	seq_printf(s, "enter_mpriv_release=%llu,", t->d.deqcmd.enter_mpriv_release_time);
	seq_printf(s, "mpriv_release=%llu,", t->d.deqcmd.mpriv_release_time);
	seq_printf(s, "sc_rets=0x%llx,", t->d.deqcmd.sc_rets);
	seq_printf(s, "c_ret=0x%llx\n", t->d.deqcmd.c_ret);
}

static int mdw_rv_tag_seq(struct seq_file *s, void *tag, void *priv)
{
	struct mdw_rv_tag *t = (struct mdw_rv_tag *)tag;

	if (!t)
		return -ENOENT;

	if (t->type == MDW_TAG_CMD)
		mdw_rv_tag_seq_cmd(s, t);
	else if (t->type == MDW_TAG_SUBCMD)
		mdw_rv_tag_seq_subcmd(s, t);
	else if (t->type == MDW_TAG_CMD_DEQUE)
		mdw_rv_tag_seq_cmd_deque(s, t);

	return 0;
}

static int mdw_rv_tag_seq_info(struct seq_file *s, void *tag, void *priv)
{
	return 0;
}

static struct apu_tp_tbl mdw_rv_tp_tbl[] = {
	{.name = "mdw_rv_cmd", .func = probe_rv_mdw_cmd},
	{.name = "mdw_rv_subcmd", .func = probe_rv_mdw_subcmd},
	{.name = "mdw_rv_cmd_deque", .func = probe_rv_mdw_cmd_deque},
	APU_TP_TBL_END
};

void mdw_rv_tag_show(struct seq_file *s)
{
	apu_tags_seq(mdw_rv_tags, s);
}

int mdw_rv_tag_init(void)
{
	int ret;

	mdw_rv_tags = apu_tags_alloc("mdw", sizeof(struct mdw_rv_tag),
		MDW_TAGS_CNT, mdw_rv_tag_seq, mdw_rv_tag_seq_info, NULL);

	if (!mdw_rv_tags)
		return -ENOMEM;

	ret = apu_tp_init(mdw_rv_tp_tbl);
	if (ret)
		pr_info("%s: unable to register\n", __func__);

	return ret;
}

void mdw_rv_tag_deinit(void)
{
	apu_tp_exit(mdw_rv_tp_tbl);
	apu_tags_free(mdw_rv_tags);
}

#else
int mdw_rv_tag_init(void)
{
	return 0;
}

void mdw_rv_tag_deinit(void)
{
}
void mdw_rv_tag_show(struct seq_file *s)
{
}
void mdw_cmd_trace(struct mdw_cmd *c, uint32_t status)
{
}
void mdw_subcmd_trace(struct mdw_cmd *c, uint32_t sc_idx, uint64_t vsid,
		uint32_t history_iptime, uint64_t sync_info, uint32_t status)
{
}
void mdw_cmd_deque_trace(struct mdw_cmd *c, uint32_t status)
{
}
#endif
