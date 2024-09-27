// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/math64.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/sched/clock.h>

#include "mdw_trace.h"
#include "mdw_cmn.h"
#include "mdw_cmd.h"
#include "mdw_mem.h"
#include "mdw_mem_pool.h"
#include "rv/mdw_rv_tag.h"

static int mdw_cmd_run(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	struct mdw_device *mdev = mpriv->mdev;
	int ret = 0;

	mdw_cmd_show(c, mdw_cmd_debug);
	mutex_lock(&c->mtx);

	c->start_ts = sched_clock();
	ret = mdev->plat_funcs->run_cmd(mpriv, c);
	if (ret) {
		mdw_drv_err("s(0x%llx) run cmd(0x%llx) fail(%d)\n",
			(uint64_t) c->mpriv, c->kid, ret);

		dma_fence_set_error(&c->fence->base_fence, ret);
	} else {
		mdw_flw_debug("s(0x%llx) cmd(0x%llx) run\n",
			(uint64_t)c->mpriv, c->kid);
	}

	mutex_unlock(&c->mtx);

	return ret;
}

static void mdw_cmd_delete_v2(struct mdw_cmd *c)
{
	struct mdw_fpriv *mpriv = c->mpriv;
	struct dma_fence *f = &c->fence->base_fence;

	mdw_cmd_show(c, mdw_drv_debug);

	mutex_lock(&mpriv->mtx);
	mdw_cmd_unvoke_map(c);
	mdw_cmd_cmdbuf_out(mpriv, c);
	mdw_cmd_delete_infos(c->mpriv, c);
	list_del(&c->u_item);
	mpriv->mdev->plat_funcs->release_cmd(mpriv);
	mutex_unlock(&mpriv->mtx);
	mdw_mem_put(c->mpriv, c->exec_infos);
	dma_fence_signal(f);
	kfree(c->adj_matrix);
	kfree(c->ksubcmds);
	kfree(c->subcmds);
	dma_fence_put(f);
	kfree(c);

	mpriv->put(mpriv);
}

static int mdw_cmd_complete(struct mdw_cmd *c, int ret)
{
	mutex_lock(&c->mtx);

	c->end_ts = sched_clock();
	c->einfos->c.total_us = div_u64(c->end_ts - c->start_ts, 1000);
	mdw_flw_debug("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d) sc_rets(0x%llx) complete, pid(%d/%d)(%d)\n",
		(uint64_t)c->mpriv, c->comm, c->uid, c->kid, c->rvid,
		ret, c->einfos->c.sc_rets,
		c->pid, c->tgid, task_pid_nr(current));

	/* check subcmds return value */
	if (c->einfos->c.sc_rets) {
		if (!ret)
			ret = -EIO;

		mdw_cmd_check_rets(c, ret);
	} else if (ret == -EBUSY) {
		mdw_exception("AP/uP busy:%s:ret(%d/0x%llx)pid(%d/%d)\n",
			c->comm, ret, c->einfos->c.sc_rets, c->pid, c->tgid);
	}
	c->einfos->c.ret = ret;

	if (ret) {
		mdw_drv_err("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d/0x%llx) time(%llu) pid(%d/%d)\n",
			(uint64_t)c->mpriv, c->comm, c->uid, c->kid, c->rvid,
			ret, c->einfos->c.sc_rets,
			c->einfos->c.total_us, c->pid, c->tgid);
		dma_fence_set_error(&c->fence->base_fence, ret);
	} else {
		mdw_flw_debug("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d/0x%llx) time(%llu) pid(%d/%d)\n",
			(uint64_t)c->mpriv, c->comm, c->uid, c->kid, c->rvid,
			ret, c->einfos->c.sc_rets,
			c->einfos->c.total_us, c->pid, c->tgid);
	}

	mutex_unlock(&c->mtx);
	mdw_cmd_delete_v2(c);

	return 0;
}

static void mdw_cmd_trigger_func(struct work_struct *wk)
{
	struct mdw_cmd *c =
		container_of(wk, struct mdw_cmd, t_wk);

	if (c->wait_fence) {
		dma_fence_wait(c->wait_fence, false);
		dma_fence_put(c->wait_fence);
	}

	mdw_flw_debug("s(0x%llx) c(0x%llx) wait fence done, start run\n",
		(uint64_t)c->mpriv, c->kid);
	mdw_cmd_run(c->mpriv, c);
}

static struct mdw_cmd *mdw_cmd_create(struct mdw_fpriv *mpriv,
	union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL;

	mdw_trace_begin("apumdw:cmd_create|s:0x%llx", (uint64_t)mpriv);

	mutex_lock(&mpriv->mtx);
	/* check num subcmds maximum */
	if (in->exec.num_subcmds > MDW_SUBCMD_MAX) {
		mdw_drv_err("too much subcmds(%u)\n", in->exec.num_subcmds);
		goto out;
	}

	/* alloc mdw cmd */
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		goto out;

	mutex_init(&c->mtx);
	INIT_LIST_HEAD(&c->map_invokes);
	c->mpriv = mpriv;

	/* setup cmd info */
	c->pid = task_pid_nr(current);
	c->tgid = current->tgid;
	c->kid = (uint64_t)c;
	c->uid = in->exec.uid;
	//c->usr_id = in->exec.usr_id;
	get_task_comm(c->comm, current);
	c->priority = in->exec.priority;
	c->hardlimit = in->exec.hardlimit;
	c->softlimit = in->exec.softlimit;
	c->power_save = in->exec.power_save;
	c->power_plcy = in->exec.power_plcy;
	c->power_dtime = in->exec.power_dtime;
	c->app_type = in->exec.app_type;
	c->num_subcmds = in->exec.num_subcmds;
	c->exec_infos = mdw_mem_get(mpriv, in->exec.exec_infos);
	if (!c->exec_infos) {
		mdw_drv_err("get exec info fail\n");
		goto free_cmd;
	}

	/* check input params */
	if (mdw_cmd_sanity_check(c)) {
		mdw_drv_err("cmd sanity check fail\n");
		goto put_execinfos;
	}

	/* subcmds/ksubcmds */
	c->subcmds = kzalloc(c->num_subcmds * sizeof(*c->subcmds), GFP_KERNEL);
	if (!c->subcmds)
		goto put_execinfos;
	if (copy_from_user(c->subcmds, (void __user *)in->exec.subcmd_infos,
		c->num_subcmds * sizeof(*c->subcmds))) {
		mdw_drv_err("copy subcmds fail\n");
		goto free_subcmds;
	}
	if (mdw_cmd_sc_sanity_check(c)) {
		mdw_drv_err("sc sanity check fail\n");
		goto free_subcmds;
	}

	c->ksubcmds = kzalloc(c->num_subcmds * sizeof(*c->ksubcmds),
		GFP_KERNEL);
	if (!c->ksubcmds)
		goto free_subcmds;

	/* adj matrix */
	c->adj_matrix = kzalloc(c->num_subcmds *
		c->num_subcmds * sizeof(uint8_t), GFP_KERNEL);
	if (!c->adj_matrix)
		goto free_ksubcmds;
	if (copy_from_user(c->adj_matrix, (void __user *)in->exec.adj_matrix,
		(c->num_subcmds * c->num_subcmds * sizeof(uint8_t)))) {
		mdw_drv_err("copy adj matrix fail\n");
		goto free_adj;
	}
	if (g_mdw_klog & MDW_DBG_CMD) {
		print_hex_dump(KERN_INFO, "[apusys] adj matrix: ",
			DUMP_PREFIX_OFFSET, 16, 1, c->adj_matrix,
			c->num_subcmds * c->num_subcmds, 0);
	}
	if (mdw_cmd_adj_check(c))
		goto free_adj;

	/* create infos */
	if (mdw_cmd_create_infos(mpriv, c)) {
		mdw_drv_err("create cmd info fail\n");
		goto free_adj;
	}

	c->mpriv->get(c->mpriv);
	c->complete = mdw_cmd_complete;
	INIT_WORK(&c->t_wk, &mdw_cmd_trigger_func);
	list_add_tail(&c->u_item, &mpriv->cmds_list);
	mdw_cmd_show(c, mdw_drv_debug);

	goto out;

free_adj:
	kfree(c->adj_matrix);
free_ksubcmds:
	kfree(c->ksubcmds);
free_subcmds:
	kfree(c->subcmds);
put_execinfos:
	mdw_mem_put(mpriv, c->exec_infos);
free_cmd:
	kfree(c);
	c = NULL;
out:
	mutex_unlock(&mpriv->mtx);
	mdw_trace_end();
	return c;
}

static int mdw_cmd_ioctl_run_v2(struct mdw_fpriv *mpriv, union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL;
	struct sync_file *sync_file = NULL;
	int ret = 0, fd = 0, wait_fd = 0;

	/* get wait fd */
	wait_fd = in->exec.fence;

	c = mdw_cmd_create(mpriv, args);
	if (!c) {
		mdw_drv_err("create cmd fail\n");
		ret = -EINVAL;
		goto out;
	}
	memset(args, 0, sizeof(*args));

	/* get sync_file fd */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		mdw_drv_err("get unused fd fail\n");
		ret = -EINVAL;
		goto delete_cmd;
	}

	/* init fence */
	if (mdw_fence_init(c, fd)) {
		mdw_drv_err("cmd init fence fail\n");
		goto put_file;
	}

	sync_file = sync_file_create(&c->fence->base_fence);
	if (!sync_file) {
		mdw_drv_err("create sync file fail\n");
		ret = -ENOMEM;
		goto put_file;
	}

	/* check wait fence from other module */
	mdw_flw_debug("s(0x%llx)c(0x%llx) wait fence(%d)\n",
			(uint64_t)c->mpriv, c->kid, wait_fd);
	c->wait_fence = sync_file_get_fence(wait_fd);
	if (!c->wait_fence) {
		mdw_flw_debug("s(0x%llx)c(0x%llx) no wait fence, trigger directly\n",
			(uint64_t)c->mpriv, c->kid);
		ret = mdw_cmd_run(mpriv, c);
	} else {
		/* wait fence from wq */
		schedule_work(&c->t_wk);
	}

	if (ret)
		goto put_file;

	/* assign fd */
	fd_install(fd, sync_file->file);
	args->out.exec.fence = fd;
	mdw_flw_debug("async fd(%d)\n", fd);
	goto out;

put_file:
	put_unused_fd(fd);
delete_cmd:
	mdw_cmd_delete_v2(c);
out:
	return ret;
}
int mdw_cmd_ioctl_v2(struct mdw_fpriv *mpriv, void *data)
{
	union mdw_cmd_args *args = (union mdw_cmd_args *)data;
	int ret = 0;

	mdw_flw_debug("s(0x%llx) op::%d\n", (uint64_t)mpriv, args->in.op);

	switch (args->in.op) {
	case MDW_CMD_IOCTL_RUN:
		ret = mdw_cmd_ioctl_run_v2(mpriv, args);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	mdw_flw_debug("done\n");

	return ret;
}

void mdw_cmd_mpriv_release_without_stale(struct mdw_fpriv *mpriv)
{
	if (!atomic_read(&mpriv->active) &&
		list_empty_careful(&mpriv->cmds_list)) {
		mdw_flw_debug("s(0x%llx) release mem\n", (uint64_t)mpriv);
		mdw_mem_mpriv_release(mpriv);
	}
}
