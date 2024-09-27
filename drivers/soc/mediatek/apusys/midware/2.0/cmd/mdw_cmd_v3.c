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
	struct dma_fence *f = &c->fence->base_fence;
	int ret = 0;

	mdw_cmd_show(c, mdw_cmd_debug);

	c->start_ts = sched_clock();
	ret = mdev->plat_funcs->run_cmd(mpriv, c);
	if (ret) {
		mdw_drv_err("s(0x%llx) run cmd(0x%llx) fail(%d)\n",
			(uint64_t) c->mpriv, c->kid, ret);
		dma_fence_set_error(f, ret);
		if (dma_fence_signal(f)) {
			mdw_drv_warn("c(0x%llx) signal fence fail\n", (uint64_t)c);
			if (f->ops->get_timeline_name && f->ops->get_driver_name) {
				mdw_drv_warn(" fence name(%s-%s)\n",
				f->ops->get_driver_name(f), f->ops->get_timeline_name(f));
			}
		}
		dma_fence_put(f);
	} else {
		mdw_flw_debug("s(0x%llx) cmd(0x%llx) run\n",
			(uint64_t)c->mpriv, c->kid);
	}

	return ret;
}

static int mdw_cmd_complete(struct mdw_cmd *c, int ret)
{
	struct dma_fence *f = &c->fence->base_fence;
	struct mdw_fpriv *mpriv = c->mpriv;

	mdw_trace_begin("apumdw:cmd_complete|cmd:0x%llx/0x%llx", c->uid, c->kid);
	mutex_lock(&c->mtx);

	c->end_ts = sched_clock();
	c->einfos->c.total_us = div_u64(c->end_ts - c->start_ts, 1000);
	mdw_flw_debug("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d) sc_rets(0x%llx) complete, pid(%d/%d)(%d)\n",
		(uint64_t)mpriv, c->comm, c->uid, c->kid, c->rvid,
		ret, c->einfos->c.sc_rets,
		c->pid, c->tgid, task_pid_nr(current));

	/* check subcmds return value */
	if (c->einfos->c.sc_rets) {
		if (!ret)
			ret = -EIO;

		mdw_cmd_check_rets(c, ret);
	} else if (ret == -EBUSY) {
		mdw_exception("uP busy:%s:ret(%d/0x%llx)pid(%d/%d)\n",
			c->comm, ret, c->einfos->c.sc_rets, c->pid, c->tgid);
	}
	c->einfos->c.ret = ret;

	if (ret) {
		mdw_drv_err("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d/0x%llx) time(%llu) pid(%d/%d)\n",
			(uint64_t)mpriv, c->comm, c->uid, c->kid, c->rvid,
			ret, c->einfos->c.sc_rets,
			c->einfos->c.total_us, c->pid, c->tgid);
		dma_fence_set_error(f, ret);

		if (mdw_debug_on(MDW_DBG_EXP))
			mdw_exception("exec fail:%s:ret(%d/0x%llx)pid(%d/%d)\n",
				c->comm, ret, c->einfos->c.sc_rets, c->pid, c->tgid);
	} else {
		mdw_flw_debug("s(0x%llx) c(%s/0x%llx/0x%llx/0x%llx) ret(%d/0x%llx) time(%llu) pid(%d/%d)\n",
			(uint64_t)mpriv, c->comm, c->uid, c->kid, c->rvid,
			ret, c->einfos->c.sc_rets,
			c->einfos->c.total_us, c->pid, c->tgid);
	}

	mdw_cmd_cmdbuf_out(mpriv, c);

	/* signal done */
	c->fence = NULL;
	atomic_dec(&c->is_running);
	if (dma_fence_signal(f)) {
		mdw_drv_warn("c(0x%llx) signal fence fail\n", (uint64_t)c);
		if (f->ops->get_timeline_name && f->ops->get_driver_name) {
			mdw_drv_warn(" fence name(%s-%s)\n",
			f->ops->get_driver_name(f), f->ops->get_timeline_name(f));
		}
	}
	dma_fence_put(f);
	atomic_dec(&mpriv->active_cmds);
	mutex_unlock(&c->mtx);

	/* check mpriv to clean cmd */
	mutex_lock(&mpriv->mtx);
	mpriv->mdev->plat_funcs->release_cmd(mpriv);
	mutex_unlock(&mpriv->mtx);

	/* put cmd execution ref */
	mdw_cmd_put(c);
	mdw_trace_end();

	return 0;
}

static void mdw_cmd_trigger_func(struct work_struct *wk)
{
	struct mdw_cmd *c =
		container_of(wk, struct mdw_cmd, t_wk);
	int ret = 0;

	if (c->wait_fence) {
		dma_fence_wait(c->wait_fence, false);
		dma_fence_put(c->wait_fence);
	}

	mdw_flw_debug("s(0x%llx) c(0x%llx) wait fence done, start run\n",
		(uint64_t)c->mpriv, c->kid);
	mutex_lock(&c->mtx);
	ret = mdw_cmd_run(c->mpriv, c);
	mutex_unlock(&c->mtx);

	/* put cmd execution ref */
	if (ret) {
		atomic_dec(&c->is_running);
		mdw_cmd_put(c);
	}
}

static struct mdw_cmd *mdw_cmd_create(struct mdw_fpriv *mpriv,
	union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL;

	mdw_trace_begin("apumdw:cmd_create|s:0x%llx", (uint64_t)mpriv);

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
	atomic_set(&c->is_running, 0);

	/* setup cmd info */
	c->pid = task_pid_nr(current);
	c->tgid = task_tgid_nr(current);
	c->kid = (uint64_t)c;
	c->uid = in->exec.uid;
	get_task_comm(c->comm, current);
	c->priority = in->exec.priority;
	c->hardlimit = in->exec.hardlimit;
	c->softlimit = in->exec.softlimit;
	c->power_save = in->exec.power_save;
	c->power_plcy = in->exec.power_plcy;
	c->power_dtime = in->exec.power_dtime;
	c->fastmem_ms = in->exec.fastmem_ms;
	c->app_type = in->exec.app_type;
	c->num_subcmds = in->exec.num_subcmds;
	c->num_links = in->exec.num_links;
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

	/* link */
	if (c->num_links) {
		c->links = kcalloc(c->num_links, sizeof(*c->links), GFP_KERNEL);
		if (!c->links)
			goto free_adj;
		if (copy_from_user(c->links, (void __user *)in->exec.links,
			c->num_links * sizeof(*c->links))) {
			mdw_drv_err("copy links fail\n");
			goto free_link;
		}
	}
	if (mdw_cmd_link_check(c))
		goto free_link;

	/* create infos */
	if (mdw_cmd_create_infos(mpriv, c)) {
		mdw_drv_err("create cmd info fail\n");
		goto free_link;
	}

	c->mpriv->get(c->mpriv);
	c->complete = mdw_cmd_complete;
	INIT_WORK(&c->t_wk, &mdw_cmd_trigger_func);
	kref_init(&c->ref);
	mdw_cmd_show(c, mdw_drv_debug);

	goto out;

free_link:
	kfree(c->links);
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
	mdw_trace_end();
	return c;
}

static int mdw_cmd_ioctl_run_v3(struct mdw_fpriv *mpriv, union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL, *priv_c = NULL;
	struct sync_file *sync_file = NULL;
	int ret = 0, fd = 0, wait_fd = 0, is_running = 0;

	mdw_trace_begin("apumdw:user_run");

	/* get wait fd */
	wait_fd = in->exec.fence;

	mutex_lock(&mpriv->mtx);
	/* get stale cmd */
	c = (struct mdw_cmd *)idr_find(&mpriv->cmds, in->id);
	if (!c) {
		/* no stale cmd, create cmd */
		mdw_cmd_debug("s(0x%llx) create new\n", (uint64_t)mpriv);
	} else if (in->op == MDW_CMD_IOCTL_RUN_STALE) {
		is_running = atomic_read(&c->is_running);
		if (is_running) {
			mdw_drv_err("s(0x%llx) c(0x%llx) is running(%d), can't execute again\n",
				(uint64_t)mpriv, (uint64_t)c, is_running);
			ret = -ETXTBSY;
			goto out;
		}
		/* run stale cmd */
		mdw_cmd_debug("s(0x%llx) run stale(0x%llx)\n",
			(uint64_t)mpriv, (uint64_t)c);
		goto exec;
	} else {
		/* release stale cmd and create new */
		mdw_cmd_debug("s(0x%llx) delete stale(0x%llx) and create new\n",
			(uint64_t)mpriv, (uint64_t)c);
		priv_c = c;
		c = NULL;
		if (priv_c != idr_remove(&mpriv->cmds, priv_c->id)) {
			mdw_drv_warn("remove cmd idr conflict(0x%llx/%d)\n",
				priv_c->kid, priv_c->id);
		}
	}

	/* create cmd */
	c = mdw_cmd_create(mpriv, args);
	if (!c) {
		mdw_drv_err("create cmd fail\n");
		ret = -EINVAL;
		goto out;
	}
	memset(args, 0, sizeof(*args));

	/* alloc idr */
	c->id = idr_alloc(&mpriv->cmds, c, MDW_CMD_IDR_MIN, MDW_CMD_IDR_MAX, GFP_KERNEL);
	if (c->id < MDW_CMD_IDR_MIN) {
		mdw_drv_err("alloc idr fail(%d)\n", c->id);
		goto delete_cmd;
	}

exec:
	mutex_lock(&c->mtx);
	mdw_cmd_trace(c, MDW_CMD_ENQUE);
	/* get sync_file fd */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		mdw_drv_err("get unused fd fail\n");
		ret = -EINVAL;
		goto delete_idr;
	}
	if (mdw_fence_init(c, fd)) {
		mdw_drv_err("cmd init fence fail\n");
		goto put_fd;
	}
	sync_file = sync_file_create(&c->fence->base_fence);
	if (!sync_file) {
		mdw_drv_err("create sync file fail\n");
		dma_fence_put(&c->fence->base_fence);
		ret = -ENOMEM;
		goto put_fd;
	}

	/* get cmd execution ref */
	atomic_inc(&c->is_running);
	mdw_cmd_get(c);

	/* check wait fence from other module */
	mdw_flw_debug("s(0x%llx)c(0x%llx) wait fence(%d)...\n",
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

	if (ret) {
		/* put cmd execution ref */
		atomic_dec(&c->is_running);
		mdw_cmd_put(c);
		goto put_file;
	}

	/* assign fd */
	fd_install(fd, sync_file->file);

	/* get ref for cmd exec */
	atomic_inc(&mpriv->active_cmds);

	/* return fd */
	args->out.exec.fence = fd;
	args->out.exec.id = c->id;
	mdw_flw_debug("async fd(%d) id(%d)\n", fd, c->id);
	mutex_unlock(&c->mtx);
	goto out;

put_file:
	fput(sync_file->file);
put_fd:
	put_unused_fd(fd);
delete_idr:
	if (c != idr_remove(&mpriv->cmds, c->id))
		mdw_drv_warn("remove cmd idr conflict(0x%llx/%d)\n", c->kid, c->id);
	mutex_unlock(&c->mtx);
delete_cmd:
	mdw_cmd_delete(c);
out:
	mutex_unlock(&mpriv->mtx);
	if (priv_c)
		mdw_cmd_delete_async(priv_c);

	mdw_trace_end();

	return ret;
}
int mdw_cmd_ioctl_v3(struct mdw_fpriv *mpriv, void *data)
{
	union mdw_cmd_args *args = (union mdw_cmd_args *)data;
	int ret = 0;

	mdw_flw_debug("s(0x%llx) op::%d\n", (uint64_t)mpriv, args->in.op);

	switch (args->in.op) {
	case MDW_CMD_IOCTL_RUN:
	case MDW_CMD_IOCTL_RUN_STALE:
		ret = mdw_cmd_ioctl_run_v3(mpriv, args);
		break;
	case MDW_CMD_IOCTL_DEL:
		ret = mdw_cmd_ioctl_del(mpriv, args);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	mdw_flw_debug("done\n");

	return ret;
}
