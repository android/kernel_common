// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include "mdw_rv.h"
#include "mdw_cmn.h"
#include "mdw_cmd.h"
#include "mdw_mem_pool.h"
#include "mdw_trace.h"

#include "linux/remoteproc/mtk_apu_ipi.h"

#define MDW_IS_HIGHADDR(addr) ((addr & 0xffffffff00000000) ? true : false)
#define MDW_POLL_TIME (1) //us

struct mdw_rv_msg_cmd {
	/* ids */
	uint64_t session_id;
	uint64_t cmd_id;
	uint64_t inference_id;
	uint32_t pid;
	uint32_t tgid;
	/* params */
	uint32_t priority;
	uint32_t hardlimit;
	uint32_t softlimit;
	uint32_t fastmem_ms;
	uint32_t power_plcy;
	uint32_t power_dtime;
	uint32_t power_etime;
	uint32_t app_type;
	uint32_t num_subcmds;
	uint32_t subcmds_offset;
	uint32_t num_cmdbufs;
	uint32_t cmdbuf_infos_offset;
	uint32_t apummu_tbl_infos_offset;
	uint32_t adj_matrix_offset;
	uint32_t exec_infos_offset;
	uint32_t num_links;
	uint32_t link_offset;
	/* history params */
	uint32_t inference_ms;
	uint32_t tolerance_ms;
	/* cmd done */
	uint32_t cmd_done;
} __packed;

struct mdw_rv_msg_sc {
	/* params */
	uint32_t type;
	uint32_t suggest_time;
	uint32_t vlm_usage;
	uint32_t vlm_ctx_id;
	uint32_t vlm_force;
	uint32_t boost;
	uint32_t turbo_boost;
	uint32_t min_boost;
	uint32_t max_boost;
	uint32_t hse_en;
	uint32_t driver_time;
	uint32_t ip_time;
	uint32_t bw;
	uint32_t pack_id;
	uint32_t affinity;
	uint32_t trigger_type;
	/* cmdbufs info */
	uint32_t cmdbuf_start_idx;
	uint32_t num_cmdbufs;
	/* history info */
	uint32_t history_ip_time;
} __packed;

struct mdw_rv_msg_cb {
	uint64_t device_va;
	uint32_t size;
} __packed;

struct mdw_rv_msg_ammu {
	uint32_t cb_head_device_va;
	uint32_t table_head_device_va;
	uint32_t table_size;
} __packed;

struct mdw_rv_sc_link {
	uint32_t producer_idx;
	uint32_t consumer_idx;
	uint32_t vid;
	uint64_t va;
	uint64_t x;
	uint64_t y;
} __packed;

static void mdw_rv_cmd_print(struct mdw_rv_msg_cmd *rc)
{
	mdw_cmd_debug("-------------------------\n");
	mdw_cmd_debug("rc kid(0x%llx)\n", rc->cmd_id);
	mdw_cmd_debug(" inference_id = 0x%llx\n", rc->inference_id);
	mdw_cmd_debug(" session = 0x%llx\n", rc->session_id);
	mdw_cmd_debug(" priority = %u\n", rc->priority);
	mdw_cmd_debug(" hardlimit = %u\n", rc->hardlimit);
	mdw_cmd_debug(" softlimit = %u\n", rc->softlimit);
	mdw_cmd_debug(" fastmem_ms = %u\n", rc->fastmem_ms);
	mdw_cmd_debug(" power_plcy = %u\n", rc->power_plcy);
	mdw_cmd_debug(" power_dtime = %u\n", rc->power_dtime);
	mdw_cmd_debug(" power_etime = %u\n", rc->power_etime);
	mdw_cmd_debug(" app_type = %u\n", rc->app_type);
	mdw_cmd_debug(" num_subcmds = %u\n", rc->num_subcmds);
	mdw_cmd_debug(" subcmds_offset = 0x%x\n", rc->subcmds_offset);
	mdw_cmd_debug(" num_cmdbufs = %u\n", rc->num_cmdbufs);
	mdw_cmd_debug(" cmdbuf_infos_offset = 0x%x\n", rc->cmdbuf_infos_offset);
	mdw_cmd_debug(" apummu_tbl_infos_offset = 0x%x\n", rc->apummu_tbl_infos_offset);
	mdw_cmd_debug(" adj_matrix_offset = 0x%x\n", rc->adj_matrix_offset);
	mdw_cmd_debug(" exec_infos_offset = 0x%x\n", rc->exec_infos_offset);
	mdw_cmd_debug(" num_links = %u\n", rc->num_links);
	mdw_cmd_debug(" link_offset = 0x%x\n", rc->link_offset);
	mdw_cmd_debug(" inference_time = %u\n", rc->inference_ms);
	mdw_cmd_debug(" tolerance_time = %u\n", rc->tolerance_ms);
	mdw_cmd_debug("-------------------------\n");
}

static void mdw_rv_sc_print(struct mdw_rv_msg_sc *rsc,
	uint64_t cmd_id, uint32_t idx)
{
	mdw_cmd_debug("-------------------------\n");
	mdw_cmd_debug("rv subcmd(0x%llx-#%u)\n", cmd_id, idx);
	mdw_cmd_debug(" type = %u\n", rsc->type);
	mdw_cmd_debug(" suggest_time = %u\n", rsc->suggest_time);
	mdw_cmd_debug(" vlm_usage = %u\n", rsc->vlm_usage);
	mdw_cmd_debug(" vlm_ctx_id = %u\n", rsc->vlm_ctx_id);
	mdw_cmd_debug(" vlm_force = %u\n", rsc->vlm_force);
	mdw_cmd_debug(" boost = %u\n", rsc->boost);
	mdw_cmd_debug(" turbo_boost = %u\n", rsc->turbo_boost);
	mdw_cmd_debug(" min_boost = %u\n", rsc->min_boost);
	mdw_cmd_debug(" max_boost = %u\n", rsc->max_boost);
	mdw_cmd_debug(" hse_en = %u\n", rsc->hse_en);
	mdw_cmd_debug(" driver_time = %u\n", rsc->driver_time);
	mdw_cmd_debug(" ip_time = %u\n", rsc->ip_time);
	mdw_cmd_debug(" pack_id = %u\n", rsc->pack_id);
	mdw_cmd_debug(" affinity = 0x%x\n", rsc->affinity);
	mdw_cmd_debug(" trigger_type = %u\n", rsc->trigger_type);
	mdw_cmd_debug(" cmdbuf_start_idx = %u\n", rsc->cmdbuf_start_idx);
	mdw_cmd_debug(" num_cmdbufs = %u\n", rsc->num_cmdbufs);
	mdw_cmd_debug(" history_ip_time = %u\n", rsc->history_ip_time);
	mdw_cmd_debug("-------------------------\n");
}

static int mdw_rv_cmd_delete(struct mdw_cmd *c)
{
	struct mdw_rv_cmd *rc = (struct mdw_rv_cmd *)c->internal_cmd;

	if (!rc)
		return -EINVAL;

	mdw_trace_begin("apumdw:rv_cmd_delete");
	mdw_mem_pool_free(rc->cb);
	kfree(rc);
	c->internal_cmd = NULL;
	c->del_internal = NULL;
	mdw_trace_end();

	return 0;
}

static struct mdw_rv_cmd *mdw_rv_cmd_create(struct mdw_fpriv *mpriv,
	struct mdw_cmd *c)
{
	struct mdw_rv_cmd *rc = NULL;
	uint64_t cb_size = 0;
	uint32_t acc_cb = 0, i = 0, j = 0;
	uint32_t subcmds_ofs = 0, cmdbuf_infos_ofs = 0, adj_matrix_ofs = 0;
	uint32_t exec_infos_ofs = 0, link_ofs = 0;
	uint32_t apummu_tbl_infos_ofs = 0;
	struct mdw_rv_msg_cmd *rmc = NULL;
	struct mdw_rv_msg_sc *rmsc = NULL;
	struct mdw_rv_msg_cb *rmcb = NULL;
	struct mdw_rv_msg_ammu *rmammu = NULL;
	struct mdw_rv_sc_link *rl = NULL;
	struct mdw_cmd_history_tbl *ch_tbl = NULL;

	mdw_trace_begin("apumdw:rv_cmd_create");
	/* reuse internal cmd if exist */
	if (c->internal_cmd) {
		mdw_cmd_debug("reuse internal cmd\n");
		rc = (struct mdw_rv_cmd *)c->internal_cmd;
		rmc = (struct mdw_rv_msg_cmd *)rc->cb->vaddr;
		goto reuse;
	}

	/* check mem address for rv */
	if (MDW_IS_HIGHADDR(c->exec_infos->device_va) ||
		MDW_IS_HIGHADDR(c->cmdbufs->device_va)) {
		mdw_drv_err("rv dva high addr(0x%llx/0x%llx)\n",
			c->cmdbufs->device_va, c->exec_infos->device_va);
		goto out;
	}

	rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	if (!rc)
		goto out;

	c->rvid = (uint64_t)&rc->s_msg;
	init_completion(&rc->s_msg.cmplt);

	/* calc size and offset */
	rc->c = c;
	cb_size += sizeof(struct mdw_rv_msg_cmd);
	cb_size = MDW_ALIGN(cb_size, MDW_DEFAULT_ALIGN);
	adj_matrix_ofs = cb_size;
	cb_size += (c->num_subcmds * c->num_subcmds * sizeof(uint8_t));
	cb_size = MDW_ALIGN(cb_size, MDW_DEFAULT_ALIGN);
	subcmds_ofs = cb_size;
	cb_size += (c->num_subcmds * sizeof(struct mdw_rv_msg_sc));
	cb_size = MDW_ALIGN(cb_size, MDW_DEFAULT_ALIGN);
	cmdbuf_infos_ofs = cb_size;
	cb_size += (c->num_cmdbufs * sizeof(struct mdw_rv_msg_cb));
	if (cb_size < (c->num_cmdbufs * sizeof(struct mdw_rv_msg_cb))) {
		mdw_drv_err("cb_size overflow(%llu) cmdbufs(%u*%zu)\n",
			cb_size, c->num_cmdbufs, sizeof(struct mdw_rv_msg_cb));
		goto free_rc;
	}
	cb_size = MDW_ALIGN(cb_size, MDW_DEFAULT_ALIGN);
	apummu_tbl_infos_ofs = cb_size;       /* APUMMU add tail of cmd buf */
	cb_size += sizeof(struct mdw_rv_msg_ammu);
	exec_infos_ofs = cb_size;
	cb_size += c->exec_infos->size;
	if (cb_size < c->exec_infos->size) {
		mdw_drv_err("cb_size overflow(%llu) exec_infos size(%llu)\n",
			cb_size, c->exec_infos->size);
		goto free_rc;
	}
	if (c->num_links) {
		link_ofs = cb_size;
		cb_size += (c->num_links * sizeof(struct mdw_rv_sc_link));
		if (cb_size < (c->num_links * sizeof(struct mdw_rv_sc_link))) {
			mdw_drv_err("cb_size overflow(%llu) links(%u*%zu)\n",
				cb_size, c->num_links, sizeof(struct mdw_rv_sc_link));
			goto free_rc;
		}
	}

	/* allocate communicate buffer */
	rc->cb = mdw_mem_pool_alloc(&mpriv->cmd_buf_pool, cb_size,
		MDW_DEFAULT_ALIGN);
	if (!rc->cb) {
		mdw_drv_err("c(0x%llx) alloc cb size(%llu) fail\n",
			c->kid, cb_size);
		goto free_rc;
	}

	/* assign cmd info */
	rmc = (struct mdw_rv_msg_cmd *)rc->cb->vaddr;
	rmc->session_id = (uint64_t)c->mpriv;
	rmc->cmd_id = c->kid;
	rmc->pid = (uint32_t)c->pid;
	rmc->tgid = (uint32_t)c->tgid;
	rmc->priority = c->priority;
	rmc->hardlimit = c->hardlimit;
	rmc->softlimit = c->softlimit;
	rmc->fastmem_ms = c->fastmem_ms;
	rmc->power_plcy = c->power_plcy;
	rmc->power_dtime = c->power_dtime;
	rmc->power_etime = c->power_etime;
	rmc->app_type = c->app_type;
	rmc->num_subcmds = c->num_subcmds;
	rmc->num_cmdbufs = c->num_cmdbufs;
	rmc->subcmds_offset = subcmds_ofs;
	rmc->cmdbuf_infos_offset = cmdbuf_infos_ofs;
	rmc->apummu_tbl_infos_offset = apummu_tbl_infos_ofs; /* APUMMU Table offset */
	rmc->adj_matrix_offset = adj_matrix_ofs;
	rmc->exec_infos_offset = exec_infos_ofs;
	rmc->num_links = c->num_links;
	rmc->link_offset = link_ofs;
	rmc->inference_ms = c->inference_ms;
	rmc->tolerance_ms = c->tolerance_ms;
	rmc->inference_id = c->inference_id;
	mdw_rv_cmd_print(rmc);

	/* copy links */
	rl = (void *)rmc + rmc->link_offset;
	for (i = 0; i < c->num_links; i++) {
		rl[i].producer_idx = c->links[i].producer_idx;
		rl[i].consumer_idx = c->links[i].consumer_idx;
		rl[i].vid = c->links[i].vid;
		rl[i].va = c->links[i].va;
		rl[i].x = c->links[i].x;
		rl[i].y = c->links[i].y;
	}

	/* assign subcmds info */
	rmsc = (void *)rmc + rmc->subcmds_offset;
	rmcb = (void *)rmc + rmc->cmdbuf_infos_offset;
	for (i = 0; i < c->num_subcmds; i++) {
		rmsc[i].type = c->subcmds[i].type;
		rmsc[i].suggest_time = c->subcmds[i].suggest_time;
		rmsc[i].vlm_usage = c->subcmds[i].vlm_usage;
		rmsc[i].vlm_ctx_id = c->subcmds[i].vlm_ctx_id;
		rmsc[i].vlm_force = c->subcmds[i].vlm_force;
		rmsc[i].boost = c->subcmds[i].boost;
		rmsc[i].ip_time = c->subcmds[i].ip_time;
		rmsc[i].driver_time = c->subcmds[i].driver_time;
		rmsc[i].bw = c->subcmds[i].bw;
		rmsc[i].turbo_boost = c->subcmds[i].turbo_boost;
		rmsc[i].min_boost = c->subcmds[i].min_boost;
		rmsc[i].max_boost = c->subcmds[i].max_boost;
		rmsc[i].hse_en = c->subcmds[i].hse_en;
		rmsc[i].pack_id = c->subcmds[i].pack_id;
		rmsc[i].affinity = c->subcmds[i].affinity;
		rmsc[i].num_cmdbufs = c->subcmds[i].num_cmdbufs;
		rmsc[i].cmdbuf_start_idx = acc_cb;
		rmsc[i].trigger_type = c->subcmds[i].trigger_type;

		for (j = 0; j < rmsc[i].num_cmdbufs; j++) {
			rmcb[acc_cb + j].size =
				c->ksubcmds[i].cmdbufs[j].size;
			rmcb[acc_cb + j].device_va =
				c->ksubcmds[i].daddrs[j];
			mdw_cmd_debug("sc(%u) #%u-cmdbufs 0x%llx/%u\n",
				i, j,
				rmcb[acc_cb + j].device_va,
				rmcb[acc_cb + j].size);
		}
		acc_cb += c->subcmds[i].num_cmdbufs;
	}

	/* assign apummu info */
	rmammu = (void *)rmc + rmc->apummu_tbl_infos_offset;
	rmammu->cb_head_device_va =  (uint32_t)mpriv->cb_head_device_va;
	rmammu->table_head_device_va =  c->cmdbufs->tbl_daddr;
	rmammu->table_size =  (uint32_t)c->size_apummutable;

	/* setup internal cmd */
	c->del_internal = mdw_rv_cmd_delete;
	c->internal_cmd = rc;

reuse:
	/* set start timestamp */
	rc->start_ts_ns = c->start_ts;

	/* copy adj matrix */
	memcpy((void *)rmc + rmc->adj_matrix_offset, c->adj_matrix,
		c->num_subcmds * c->num_subcmds * sizeof(uint8_t));

	/* clear einfos */
	memset((void *)rmc + rmc->exec_infos_offset, 0, c->exec_infos->size);

	/* clear exec ret */
	c->einfos->c.ret = 0;
	c->einfos->c.sc_rets = 0;
	rmc->cmd_done = 0;

	/* update history ip time */
	rmsc = (void *)rmc + rmc->subcmds_offset;
	ch_tbl = mdw_cmd_ch_tbl_find(c);
	for (i = 0; i < c->num_subcmds; i++) {
		if (ch_tbl)
			rmsc[i].history_ip_time = ch_tbl->h_sc_einfo[i].ip_time;
		mdw_rv_sc_print(&rmsc[i], rmc->cmd_id, i);
	}

	if (mdw_mem_flush(mpriv, rc->cb))
		mdw_drv_warn("s(0x%llx) c(0x%llx/0x%llx) flush rv cbs(%llu) fail\n",
			(uint64_t)c->mpriv, c->kid, c->rvid, rc->cb->size);


	goto out;

free_rc:
	kfree(rc);
	rc = NULL;
out:
	mdw_trace_end();
	return rc;
}

static void mdw_rv_cmd_cp_execinfo(struct mdw_rv_cmd *rc)
{
	struct mdw_cmd *c = rc->c;
	struct mdw_rv_msg_cmd *rmc = NULL;

	/* invalidate */
	if (mdw_mem_invalidate(c->mpriv, rc->cb))
		mdw_drv_warn("s(0x%llx) c(0x%llx/0x%llx/0x%llx) invalidate rcbs(%llu) fail\n",
			(uint64_t)c->mpriv, c->uid, c->kid,
			c->rvid, rc->cb->size);

	/* copy exec infos */
	rmc = (struct mdw_rv_msg_cmd *)rc->cb->vaddr;
	if (rmc->exec_infos_offset + c->exec_infos->size == rc->cb->size ||
		rmc->link_offset + c->num_links * sizeof(struct mdw_rv_sc_link)
		== rc->cb->size) {
		memcpy(c->exec_infos->vaddr,
			rc->cb->vaddr + rmc->exec_infos_offset,
			c->exec_infos->size);
	} else {
		mdw_drv_warn("c(0x%llx/0x%llx/0x%llx) execinfos(%llu/%u) links(%u/%llu) not matched\n",
			c->uid, c->kid, c->rvid,
			rmc->exec_infos_offset + c->exec_infos->size,
			rmc->link_offset, c->num_links,
			rc->cb->size);
	}
}

static void mdw_rv_cmd_done(struct mdw_rv_cmd *rc, int ret)
{
	struct mdw_cmd *c = rc->c;

	/* complete cmd */
	c->complete(c, ret);
}

static bool mdw_rv_cmd_poll(struct mdw_rv_cmd *rc)
{
	struct mdw_rv_msg_cmd *rmc = NULL;
	uint32_t i = 0;
	uint32_t poll_interval = MDW_POLL_TIME, poll_timeout = MDW_POLL_TIMEOUT;
	bool poll_ret = false;

	rmc = (struct mdw_rv_msg_cmd *)rc->cb->vaddr;

	if (g_mdw_poll_interval)
		poll_interval = g_mdw_poll_interval;
	if (g_mdw_poll_timeout)
		poll_timeout = g_mdw_poll_timeout;

	mdw_cmd_debug("poll_interval(%u), poll_timeout(%u)\n", poll_interval, poll_timeout);

	/* poll cmd done result */
	for(i = 0; i< poll_timeout; i++) {
		if(rmc->cmd_done) {
			poll_ret = true;
			break;
		}
		udelay(poll_interval);
	}
	return poll_ret;
}

/* kernel-tinysys version v4 */
const struct mdw_rv_cmd_func mdw_rv_cmd_func_v4 = {
	.create = mdw_rv_cmd_create,
	.delete = mdw_rv_cmd_delete,
	.done = mdw_rv_cmd_done,
	.poll = mdw_rv_cmd_poll,
	.cp_execinfo = mdw_rv_cmd_cp_execinfo,
};
