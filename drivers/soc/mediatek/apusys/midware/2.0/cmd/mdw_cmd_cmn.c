// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/sched/clock.h>
#include <linux/min_heap.h>

#include "mdw_trace.h"
#include "mdw_cmn.h"
#include "mdw_cmd.h"
#include "mdw_mem.h"
#include "mdw_mem_pool.h"
#include "linux/soc/mediatek/apummu_export.h"
#include "rv/mdw_rv_tag.h"
#include "mdw_ext_export.h"
#include "linux/soc/mediatek/apu_mem_export.h"
#include "linux/soc/mediatek/apu_mem_def.h"

void mdw_cmd_cmdbuf_out(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	struct mdw_subcmd_kinfo *ksubcmd = NULL;
	unsigned int i = 0, j = 0;

	if (!c->cmdbufs)
		return;

	/* flush cmdbufs and execinfos */
	if (mdw_mem_invalidate(mpriv, c->cmdbufs))
		mdw_drv_warn("s(0x%llx)c(0x%llx) invalidate cmdbufs(%llu) fail\n",
			(uint64_t)mpriv, c->kid, c->cmdbufs->size);

	for (i = 0; i < c->num_subcmds; i++) {
		ksubcmd = &c->ksubcmds[i];
		for (j = 0; j < ksubcmd->info->num_cmdbufs; j++) {
			if (!ksubcmd->ori_cbs[j]) {
				mdw_drv_warn("no ori mems(%d-%d)\n", i, j);
				continue;
			}

			/* cmdbuf copy out */
			if (ksubcmd->cmdbufs[j].direction != MDW_CB_IN) {
				mdw_trace_begin("apumdw:cbs_copy_out|cb:%u-%u size:%llu type:%u",
					i, j, ksubcmd->ori_cbs[j]->size, ksubcmd->info->type);
				memcpy(ksubcmd->ori_cbs[j]->vaddr,
					(void *)ksubcmd->kvaddrs[j],
					ksubcmd->ori_cbs[j]->size);
				mdw_trace_end();
			}
		}
	}
}

void mdw_cmd_put_cmdbufs(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	struct mdw_subcmd_kinfo *ksubcmd = NULL;
	unsigned int i = 0, j = 0;

	if (!c->cmdbufs)
		return;

	mdw_trace_begin("apumdw:cbs_put|c:0x%llx num_subcmds:%u num_cmdbufs:%u",
		c->kid, c->num_subcmds, c->num_cmdbufs);

	for (i = 0; i < c->num_subcmds; i++) {
		ksubcmd = &c->ksubcmds[i];
		for (j = 0; j < ksubcmd->info->num_cmdbufs; j++) {
			if (!ksubcmd->ori_cbs[j]) {
				mdw_drv_warn("no ori mems(%d-%d)\n", i, j);
				continue;
			}

			/* put mem */
			mdw_mem_put(mpriv, ksubcmd->ori_cbs[j]);
			ksubcmd->ori_cbs[j] = NULL;
		}
	}

	mdw_mem_pool_free(c->cmdbufs);
	c->cmdbufs = NULL;

	mdw_trace_end();
}

int mdw_cmd_get_cmdbufs(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	unsigned int i = 0, j = 0, ofs = 0;
	int ret = -EINVAL;
	struct mdw_subcmd_kinfo *ksubcmd = NULL;
	struct mdw_mem *m = NULL;
	struct apusys_cmdbuf *acbs = NULL;

	mdw_trace_begin("apumdw:cbs_get|c:0x%llx num_subcmds:%u num_cmdbufs:%u",
		c->kid, c->num_subcmds, c->num_cmdbufs);

	if (!c->size_cmdbufs || c->cmdbufs)
		goto out;

	c->cmdbufs = mdw_mem_pool_alloc(&mpriv->cmd_buf_pool, c->size_cmdbufs,
		MDW_DEFAULT_ALIGN);
	if (!c->cmdbufs) {
		mdw_drv_err("s(0x%llx)c(0x%llx) alloc buffer for duplicate fail\n",
		(uint64_t) mpriv, c->kid);
		ret = -ENOMEM;
		goto out;
	}

	/* alloc mem for duplicated cmdbuf */
	for (i = 0; i < c->num_subcmds; i++) {
		mdw_cmd_debug("sc(0x%llx-%u) #cmdbufs(%u)\n",
			c->kid, i, c->ksubcmds[i].info->num_cmdbufs);

		acbs = kcalloc(c->ksubcmds[i].info->num_cmdbufs, sizeof(*acbs), GFP_KERNEL);
		if (!acbs)
			goto free_cmdbufs;

		ksubcmd = &c->ksubcmds[i];
		for (j = 0; j < ksubcmd->info->num_cmdbufs; j++) {
			/* calc align */
			if (ksubcmd->cmdbufs[j].align)
				ofs = MDW_ALIGN(ofs, ksubcmd->cmdbufs[j].align);
			else
				ofs = MDW_ALIGN(ofs, MDW_DEFAULT_ALIGN);

			mdw_cmd_debug("sc(0x%llx-%u) cb#%u offset(%u)\n",
				c->kid, i, j, ofs);

			/* get mem from handle */
			m = mdw_mem_get(mpriv, ksubcmd->cmdbufs[j].handle);
			if (!m) {
				mdw_drv_err("sc(0x%llx-%u) cb#%u(%llu) get fail\n",
					c->kid, i, j,
					ksubcmd->cmdbufs[j].handle);
				ret = -EINVAL;
				goto free_cmdbufs;
			}
			/* check mem boundary */
			if (m->vaddr == NULL ||
				ksubcmd->cmdbufs[j].size != m->size) {
				mdw_drv_err("sc(0x%llx-%u) cb#%u invalid range(%p/%u/%llu)\n",
					c->kid, i, j, m->vaddr,
					ksubcmd->cmdbufs[j].size,
					m->size);
				ret = -EINVAL;
				goto free_cmdbufs;
			}

			/* cmdbuf copy in */
			if (ksubcmd->cmdbufs[j].direction != MDW_CB_OUT) {
				mdw_trace_begin("apumdw:cbs_copy_in|cb:%u-%u size:%u type:%u",
					i, j,
					ksubcmd->cmdbufs[j].size,
					ksubcmd->info->type);
				memcpy(c->cmdbufs->vaddr + ofs,
					m->vaddr,
					ksubcmd->cmdbufs[j].size);
				mdw_trace_end();
			}

			/* record buffer info */
			ksubcmd->ori_cbs[j] = m;
			ksubcmd->kvaddrs[j] =
				(uint64_t)(c->cmdbufs->vaddr + ofs);
			ksubcmd->daddrs[j] =
				(uint64_t)(c->cmdbufs->device_va + ofs);
			ofs += ksubcmd->cmdbufs[j].size;

			mdw_cmd_debug("sc(0x%llx-%u) cb#%u (0x%llx/0x%llx/%u)\n",
				c->kid, i, j,
				ksubcmd->kvaddrs[j],
				ksubcmd->daddrs[j],
				ksubcmd->cmdbufs[j].size);

			acbs[j].kva = (void *)ksubcmd->kvaddrs[j];
			acbs[j].size = ksubcmd->cmdbufs[j].size;
		}

		mdw_trace_begin("apumdw:dev validation|c:0x%llx type:%u",
			c->kid, ksubcmd->info->type);
		ret = mdw_dev_validation(mpriv, ksubcmd->info->type,
			c, acbs, ksubcmd->info->num_cmdbufs);
		mdw_trace_end();
		kfree(acbs);
		acbs = NULL;
		if (ret) {
			mdw_drv_err("sc(0x%llx-%u) dev(%u) validate cb(%u) fail(%d)\n",
				c->kid, i, ksubcmd->info->type, ksubcmd->info->num_cmdbufs, ret);
			goto free_cmdbufs;
		}
	}

	/* flush cmdbufs */
	if (mdw_mem_flush(mpriv, c->cmdbufs))
		mdw_drv_warn("s(0x%llx) c(0x%llx) flush cmdbufs(%llu) fail\n",
			(uint64_t)mpriv, c->kid, c->cmdbufs->size);

	ret = 0;
	goto out;

free_cmdbufs:
	mdw_cmd_put_cmdbufs(mpriv, c);
	kfree(acbs);
out:
	mdw_cmd_debug("ret(%d)\n", ret);
	mdw_trace_end();
	return ret;
}

int mdw_cmd_get_apummutable(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	unsigned int tbl_size = 0;
	int ret = -ENOMEM;

	mdw_cmd_debug("mdw call apummu_table_get priv(0x%llx) tbl_kva(0x%llx)\n",
	 (uint64_t)c->mpriv, (uint64_t)c->tbl_kva);

	/* get apummu table */
	mdw_trace_begin("apummu:table_get|s:0x%llx", (uint64_t)c->mpriv);
	ret = apu_mem_table_get((uint64_t)c->mpriv, &c->tbl_kva, &tbl_size);
	mdw_trace_end();

	if (ret == -EOPNOTSUPP) {
		mdw_drv_warn("no support apummu\n");
		return ret;
	}
	if (ret) {
		mdw_drv_err("get apummu table fail\n");
		return ret;
	}

	c->size_cmdbufs += tbl_size;
	c->size_apummutable = tbl_size;
	mdw_cmd_debug("c->kid(0x%llx) tbl_size(%u) apummutable size(%u)\n",
		c->kid, tbl_size, c->size_apummutable);

	return ret;
}

int mdw_cmd_create_infos(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	unsigned int i = 0, j = 0, total_size = 0, tmp_size = 0;
	struct mdw_subcmd_exec_info *sc_einfo = NULL;
	int ret = -ENOMEM;

	c->einfos = c->exec_infos->vaddr;
	if (!c->einfos) {
		mdw_drv_err("invalid exec info addr\n");
		return -EINVAL;
	}
	/* clear run infos for return */
	memset(c->exec_infos->vaddr, 0, c->exec_infos->size);

	sc_einfo = &c->einfos->sc;

	for (i = 0; i < c->num_subcmds; i++) {
		c->ksubcmds[i].info = &c->subcmds[i];
		mdw_cmd_debug("subcmd(%u)(%u/%u/%u/%u/%u/%u/%u/%u/0x%llx)\n",
			i, c->subcmds[i].type,
			c->subcmds[i].suggest_time, c->subcmds[i].vlm_usage,
			c->subcmds[i].vlm_ctx_id, c->subcmds[i].vlm_force,
			c->subcmds[i].boost, c->subcmds[i].pack_id,
			c->subcmds[i].num_cmdbufs, c->subcmds[i].cmdbufs);

		/* kva for oroginal buffer */
		c->ksubcmds[i].ori_cbs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(c->ksubcmds[i].ori_cbs), GFP_KERNEL);
		if (!c->ksubcmds[i].ori_cbs)
			goto free_cmdbufs;

		/* record kva for duplicate */
		c->ksubcmds[i].kvaddrs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(*c->ksubcmds[i].kvaddrs), GFP_KERNEL);
		if (!c->ksubcmds[i].kvaddrs)
			goto free_cmdbufs;

		/* record dva for cmdbufs */
		c->ksubcmds[i].daddrs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(*c->ksubcmds[i].daddrs), GFP_KERNEL);
		if (!c->ksubcmds[i].daddrs)
			goto free_cmdbufs;

		/* allocate for subcmd cmdbuf */
		c->ksubcmds[i].cmdbufs = kcalloc(c->subcmds[i].num_cmdbufs,
			sizeof(*c->ksubcmds[i].cmdbufs), GFP_KERNEL);
		if (!c->ksubcmds[i].cmdbufs)
			goto free_cmdbufs;

		/* copy cmdbuf info */
		if (copy_from_user(c->ksubcmds[i].cmdbufs,
			(void __user *)c->subcmds[i].cmdbufs,
			c->subcmds[i].num_cmdbufs *
			sizeof(*c->ksubcmds[i].cmdbufs))) {
			goto free_cmdbufs;
		}

		c->ksubcmds[i].sc_einfo = &sc_einfo[i];

		/* accumulate cmdbuf size with alignment */
		for (j = 0; j < c->subcmds[i].num_cmdbufs; j++) {
			c->num_cmdbufs++;
			/* alignment */
			if (c->ksubcmds[i].cmdbufs[j].align) {
				tmp_size = MDW_ALIGN(total_size,
					c->ksubcmds[i].cmdbufs[j].align);
			} else {
				tmp_size = MDW_ALIGN(total_size, MDW_DEFAULT_ALIGN);
			}
			if (tmp_size < total_size) {
				mdw_drv_err("cmdbuf(%u,%u) size align overflow(%u/%u/%u)\n",
					i, j, total_size,
					c->ksubcmds[i].cmdbufs[j].align, tmp_size);
				goto free_cmdbufs;
			}
			total_size = tmp_size;

			/* accumulator */
			tmp_size = total_size + c->ksubcmds[i].cmdbufs[j].size;
			if (tmp_size < total_size) {
				mdw_drv_err("cmdbuf(%u,%u) size overflow(%u/%u/%u)\n",
					i, j, total_size,
					c->ksubcmds[i].cmdbufs[j].size, tmp_size);
				goto free_cmdbufs;
			}
			total_size = tmp_size;
		}
	}
	/* align cmdbuf tail offset for apummu table */
	tmp_size = MDW_ALIGN(total_size, MDW_DEFAULT_ALIGN);
	if (tmp_size < total_size) {
		mdw_drv_err("cmdbuf end size align overflow(%u/%u)\n",
			total_size, tmp_size);
		goto free_cmdbufs;
	}
	total_size = tmp_size;
	c->size_cmdbufs = total_size;

	mdw_cmd_debug("sc(0x%llx) cb_num(%u) total size(%u)\n",
		c->kid, c->num_cmdbufs, c->size_cmdbufs);

	ret = mdw_cmd_get_apummutable(mpriv, c);
	if (ret && ret != -EOPNOTSUPP) {
		mdw_drv_err("get apummu table fail\n");
		goto free_cmdbufs;
	}

	ret = mpriv->mdev->plat_funcs->get_cmdbuf(mpriv, c);
	if (ret)
		goto free_cmdbufs;

	goto out;

free_cmdbufs:
	for (i = 0; i < c->num_subcmds; i++) {
		/* free dvaddrs */
		kfree(c->ksubcmds[i].daddrs);
		c->ksubcmds[i].daddrs = NULL;

		/* free kvaddrs */
		kfree(c->ksubcmds[i].kvaddrs);
		c->ksubcmds[i].kvaddrs = NULL;

		/* free ori kvas */
		kfree(c->ksubcmds[i].ori_cbs);
		c->ksubcmds[i].ori_cbs = NULL;

		/* free cmdbufs */
		kfree(c->ksubcmds[i].cmdbufs);
		c->ksubcmds[i].cmdbufs = NULL;
	}

out:
	return ret;
}

void mdw_cmd_delete_infos(struct mdw_fpriv *mpriv, struct mdw_cmd *c)
{
	unsigned int i = 0;

	mdw_cmd_put_cmdbufs(mpriv, c);

	for (i = 0; i < c->num_subcmds; i++) {
		/* free dvaddrs */
		kfree(c->ksubcmds[i].daddrs);
		c->ksubcmds[i].daddrs = NULL;

		/* free kvaddrs */
		kfree(c->ksubcmds[i].kvaddrs);
		c->ksubcmds[i].kvaddrs = NULL;

		/* free ori kvas */
		kfree(c->ksubcmds[i].ori_cbs);
		c->ksubcmds[i].ori_cbs = NULL;

		/* free cmdbufs */
		kfree(c->ksubcmds[i].cmdbufs);
		c->ksubcmds[i].cmdbufs = NULL;
	}
}

void mdw_cmd_history_reset(struct mdw_fpriv *mpriv)
{
	struct mdw_device *mdev = mpriv->mdev;
	int i = 0;

	/* reset min heap */
	mutex_lock(&mdev->h_mtx);
	mdev->heap.nr = 0;
	for (i = 0; i < MDW_NUM_PREDICT_CMD; i++)
		mdev->predict_cmd_ts[i] = 0;
	mutex_unlock(&mdev->h_mtx);
}

static void mdw_cmd_history_tbl_delete(struct mdw_fpriv *mpriv)
{
	struct mdw_cmd_history_tbl *ch_tbl = NULL, *tmp = NULL;

	mutex_lock(&mpriv->ch_mtx);
	list_for_each_entry_safe(ch_tbl, tmp, &mpriv->ch_list, ch_tbl_node) {
		list_del(&ch_tbl->ch_tbl_node);
		mdw_cmd_debug("s(0x%llx) uid(0x%llx) delete ch_tbl\n",
			(uint64_t)mpriv, ch_tbl->uid);
		kfree(ch_tbl->h_sc_einfo);
		kfree(ch_tbl);
	}
	mutex_unlock(&mpriv->ch_mtx);

	mdw_cmd_history_reset(mpriv);
}

void mdw_cmd_mpriv_release(struct mdw_fpriv *mpriv)
{
	struct mdw_cmd *c = NULL;
	uint32_t id = 0;

	if (!atomic_read(&mpriv->active) && !atomic_read(&mpriv->active_cmds)) {
		mdw_flw_debug("s(0x%llx) release cmd\n", (uint64_t)mpriv);
		idr_for_each_entry(&mpriv->cmds, c, id) {
			idr_remove(&mpriv->cmds, id);
			mdw_cmd_delete(c);
		}
		mdw_flw_debug("s(0x%llx) release mem\n", (uint64_t)mpriv);
		mutex_lock(&mpriv->mdev->mctl_mtx);
		mdw_mem_mpriv_release(mpriv);
		mutex_unlock(&mpriv->mdev->mctl_mtx);
		mdw_flw_debug("s(0x%llx) release apummu table\n", (uint64_t)mpriv);
		mdw_trace_begin("apummu:table_free|s:0x%llx", (uint64_t)mpriv);
		apu_mem_table_free((uint64_t)mpriv);
		mdw_trace_end();
		mdw_flw_debug("s(0x%llx) release history tbl\n", (uint64_t)mpriv);
		mdw_cmd_history_tbl_delete(mpriv);
	}
}

//--------------------------------------------
uint64_t mdw_fence_ctx_alloc(struct mdw_device *mdev)
{
	uint64_t idx = 0, ctx = 0;

	mutex_lock(&mdev->f_mtx);
	idx = find_first_zero_bit(mdev->fence_ctx_mask, mdev->num_fence_ctx);
	if (idx >= mdev->num_fence_ctx) {
		ctx = dma_fence_context_alloc(1);
		mdw_drv_warn("no free fence ctx(%llu), alloc ctx(%llu)\n", idx, ctx);
	} else {
		set_bit(idx, mdev->fence_ctx_mask);
		ctx = mdev->base_fence_ctx + idx;
	}
	mutex_unlock(&mdev->f_mtx);
	mdw_cmd_debug("alloc fence ctx(%llu) idx(%llu) base(%llu)\n",
		ctx, idx, mdev->base_fence_ctx);

	return ctx;
}

void mdw_fence_ctx_free(struct mdw_device *mdev, uint64_t ctx)
{
	int idx = 0;

	idx = ctx - mdev->base_fence_ctx;
	if (idx < 0 || idx >= mdev->num_fence_ctx) {
		mdw_cmd_debug("out of range ctx(%llu/%llu)\n", ctx, mdev->base_fence_ctx);
		return;
	}

	mutex_lock(&mdev->f_mtx);
	if (!test_bit(idx, mdev->fence_ctx_mask))
		mdw_drv_warn("ctx state conflict(%d)\n", idx);
	else
		clear_bit(idx, mdev->fence_ctx_mask);
	mutex_unlock(&mdev->f_mtx);
}

const char *mdw_fence_get_driver_name(struct dma_fence *fence)
{
	return "apu_mdw";
}

const char *mdw_fence_get_timeline_name(struct dma_fence *fence)
{
	struct mdw_fence *f =
		container_of(fence, struct mdw_fence, base_fence);

	return f->name;
}

bool mdw_fence_enable_signaling(struct dma_fence *fence)
{
	return true;
}

void mdw_fence_release(struct dma_fence *fence)
{
	struct mdw_fence *mf =
		container_of(fence, struct mdw_fence, base_fence);

	mdw_flw_debug("fence release, fence(%s/%llu-%llu)\n",
		mf->name, mf->base_fence.context, mf->base_fence.seqno);
	mdw_fence_ctx_free(mf->mdev, mf->base_fence.context);
	kfree(mf);
}

const struct dma_fence_ops mdw_fence_ops = {
	.get_driver_name =  mdw_fence_get_driver_name,
	.get_timeline_name =  mdw_fence_get_timeline_name,
	.enable_signaling =  mdw_fence_enable_signaling,
	.wait = dma_fence_default_wait,
	.release =  mdw_fence_release,
};

//--------------------------------------------
int mdw_fence_init(struct mdw_cmd *c, int fd)
{
	int ret = 0;
	struct mdw_device *mdev = c->mpriv->mdev;

	c->fence = kzalloc(sizeof(*c->fence), GFP_KERNEL);
	if (!c->fence)
		return -ENOMEM;

	if (snprintf(c->fence->name, sizeof(c->fence->name), "%d:%s", fd, c->comm) <= 0)
		mdw_drv_warn("set fance name fail\n");
	c->fence->mdev = c->mpriv->mdev;
	spin_lock_init(&c->fence->lock);
	dma_fence_init(&c->fence->base_fence, &mdw_fence_ops,
		&c->fence->lock, mdw_fence_ctx_alloc(mdev),
		atomic_add_return(1, &c->mpriv->exec_seqno));

	mdw_flw_debug("fence init, c(0x%llx) fence(%s/%llu-%llu)\n",
		(uint64_t)c, c->fence->name, c->fence->base_fence.context,
		c->fence->base_fence.seqno);

	return ret;
}

void mdw_cmd_unvoke_map(struct mdw_cmd *c)
{
	struct mdw_cmd_map_invoke *cm_invoke = NULL, *tmp = NULL;

	list_for_each_entry_safe(cm_invoke, tmp, &c->map_invokes, c_node) {
		list_del(&cm_invoke->c_node);
		mdw_cmd_debug("s(0x%llx)c(0x%llx) unvoke m(0x%llx/%llu)\n",
			(uint64_t)c->mpriv, (uint64_t)c,
			cm_invoke->map->m->device_va,
			cm_invoke->map->m->dva_size);
		cm_invoke->map->put(cm_invoke->map);
		kfree(cm_invoke);
	}
}

int mdw_cmd_invoke_map(struct mdw_cmd *c, struct mdw_mem_map *map)
{
	struct mdw_cmd_map_invoke *cm_invoke = NULL;

	if (map == NULL)
		return -EINVAL;

	/* query */
	list_for_each_entry(cm_invoke, &c->map_invokes, c_node) {
		/* already invoked */
		if (cm_invoke->map == map)
			return 0;
	}

	cm_invoke = kzalloc(sizeof(*cm_invoke), GFP_KERNEL);
	if (cm_invoke == NULL)
		return -ENOMEM;

	map->get(map);
	cm_invoke->map = map;
	list_add_tail(&cm_invoke->c_node, &c->map_invokes);
	mdw_cmd_debug("s(0x%llx)c(0x%llx) invoke m(0x%llx/%llu)\n",
		(uint64_t)c->mpriv, (uint64_t)c, map->m->device_va, map->m->dva_size);

	return 0;
}

static void mdw_cmd_release(struct kref *ref)
{
	struct mdw_cmd *c =
			container_of(ref, struct mdw_cmd, ref);
	struct mdw_fpriv *mpriv = c->mpriv;

	mdw_cmd_show(c, mdw_drv_debug);
	/* remove ext id */
	mdw_ext_cmd_put_id(c);
	mdw_trace_begin("apumdw:cmd_release|c:0x%llx", c->kid);
	if (c->del_internal)
		c->del_internal(c);
	mutex_lock(&mpriv->mdev->mctl_mtx);
	mdw_cmd_unvoke_map(c);
	mutex_unlock(&mpriv->mdev->mctl_mtx);
	mdw_cmd_delete_infos(c->mpriv, c);
	mdw_mem_put(c->mpriv, c->exec_infos);
	kfree(c->adj_matrix);
	kfree(c->ksubcmds);
	kfree(c->subcmds);
	kfree(c);

	mpriv->put(mpriv);
	mdw_trace_end();
}

void mdw_cmd_put(struct mdw_cmd *c)
{
	mdw_ext_lock();
	kref_put(&c->ref, mdw_cmd_release);
	mdw_ext_unlock();
}

void mdw_cmd_get(struct mdw_cmd *c)
{
	kref_get(&c->ref);
}

void mdw_cmd_delete(struct mdw_cmd *c)
{
	mdw_cmd_show(c, mdw_drv_debug);
	mdw_cmd_put(c);
}

void mdw_cmd_delete_async(struct mdw_cmd *c)
{
	struct mdw_device *mdev = c->mpriv->mdev;

	/* add cmd list to delete */
	mutex_lock(&mdev->c_mtx);
	list_add_tail(&c->d_node, &mdev->d_cmds);
	mutex_unlock(&mdev->c_mtx);

	schedule_work(&mdev->c_wk);
}

int mdw_cmd_sanity_check(struct mdw_cmd *c)
{
	if (c->priority >= MDW_PRIORITY_MAX ||
		c->num_subcmds > MDW_SUBCMD_MAX ||
		c->num_links > c->num_subcmds) {
		mdw_drv_err("s(0x%llx)cmd invalid(0x%llx/0x%llx)(%u/%u/%u)\n",
			(uint64_t)c->mpriv, c->uid, c->kid,
			c->priority, c->num_subcmds, c->num_links);
		return -EINVAL;
	}

	if (c->exec_infos->size != sizeof(struct mdw_cmd_exec_info) +
		c->num_subcmds * sizeof(struct mdw_subcmd_exec_info)) {
		mdw_drv_err("s(0x%llx)cmd invalid(0x%llx/0x%llx) einfo(%llu/%zu)\n",
			(uint64_t)c->mpriv, c->uid, c->kid,
			c->exec_infos->size,
			sizeof(struct mdw_cmd_exec_info) +
			c->num_subcmds * sizeof(struct mdw_subcmd_exec_info));
		return -EINVAL;
	}

	return 0;
}

int mdw_cmd_adj_check(struct mdw_cmd *c)
{
	uint32_t i = 0, j = 0;

	for (i = 0; i < c->num_subcmds; i++) {
		for (j = 0; j < c->num_subcmds; j++) {
			if (i == j) {
				c->adj_matrix[i * c->num_subcmds + j] = 0;
				continue;
			}

			if (i < j)
				continue;

			if (!c->adj_matrix[i * c->num_subcmds + j] ||
				!c->adj_matrix[i + j * c->num_subcmds])
				continue;

			mdw_drv_err("s(0x%llx)c(0x%llx/0x%llx) adj matrix(%u/%u) fail\n",
				(uint64_t)c->mpriv, c->uid, c->kid, i, j);
			return -EINVAL;
		}
	}

	return 0;
}

int mdw_cmd_link_check(struct mdw_cmd *c)
{
	uint32_t i = 0;

	for (i = 0; i < c->num_links; i++) {
		if (c->links[i].producer_idx > c->num_subcmds ||
			c->links[i].consumer_idx > c->num_subcmds ||
			!c->links[i].x || !c->links[i].y ||
			!c->links[i].va) {
			mdw_drv_err("link(%u) invalid(%u/%u)(%llu/%llu)(0x%llx)\n", i,
				c->links[i].producer_idx,
				c->links[i].consumer_idx,
				c->links[i].x,
				c->links[i].y,
				c->links[i].va);
			return -EINVAL;
		}
	}
	return 0;
}

int mdw_cmd_sc_sanity_check(struct mdw_cmd *c)
{
	unsigned int i = 0;

	/* subcmd info */
	for (i = 0; i < c->num_subcmds; i++) {
		if (c->subcmds[i].type >= MDW_DEV_MAX ||
			c->subcmds[i].vlm_ctx_id >= MDW_SUBCMD_MAX ||
			c->subcmds[i].boost > MDW_BOOST_MAX ||
			c->subcmds[i].pack_id >= MDW_SUBCMD_MAX) {
			mdw_drv_err("subcmd(%u) invalid (%u/%u/%u)\n",
				i, c->subcmds[i].type,
				c->subcmds[i].boost,
				c->subcmds[i].pack_id);
			return -EINVAL;
		}
	}

	return 0;
}

void mdw_cmd_check_rets(struct mdw_cmd *c, int ret)
{
	uint32_t idx = 0, is_dma = 0, is_aps = 0;
	DECLARE_BITMAP(tmp, 64);

	memcpy(&tmp, &c->einfos->c.sc_rets, sizeof(c->einfos->c.sc_rets));

	/* extract fail subcmd */
	do {
		idx = find_next_bit((unsigned long *)&tmp, c->num_subcmds, idx);
		if (idx >= c->num_subcmds)
			break;

		mdw_drv_warn("sc(0x%llx-#%u) type(%u) softlimit(%u) boost(%u) fail\n",
			c->kid, idx, c->subcmds[idx].type,
			c->softlimit, c->subcmds[idx].boost);
		switch (c->subcmds[idx].type) {
		case APUSYS_DEVICE_EDMA:
			is_dma++;
			break;
		case APUSYS_DEVICE_APS:
			is_aps++;
			break;
		default:
			break;
		}

		idx++;
	} while (idx < c->num_subcmds);

	/* trigger exception if dma */
	if (is_dma) {
		dma_exception("dma exec fail:%s:ret(%d/0x%llx)pid(%d/%d)c(0x%llx)\n",
			c->comm, ret, c->einfos->c.sc_rets,
			c->pid, c->tgid, c->kid);
	}
	if (is_aps) {
		aps_exception("aps exec fail:%s:ret(%d/0x%llx)pid(%d/%d)c(0x%llx)\n",
			c->comm, ret, c->einfos->c.sc_rets,
			c->pid, c->tgid, c->kid);
	}
}

int mdw_cmd_ioctl_del(struct mdw_fpriv *mpriv, union mdw_cmd_args *args)
{
	struct mdw_cmd_in *in = (struct mdw_cmd_in *)args;
	struct mdw_cmd *c = NULL;
	int ret = 0;

	mdw_trace_begin("apumdw:user_delete");

	mutex_lock(&mpriv->mtx);
	c = (struct mdw_cmd *)idr_find(&mpriv->cmds, in->id);
	if (!c) {
		ret = -EINVAL;
		mdw_drv_warn("can't find id(%lld)\n", in->id);
	} else {
		if (c != idr_remove(&mpriv->cmds, c->id))
			mdw_drv_warn("remove cmd idr conflict(0x%llx/%d)\n", c->kid, c->id);
		mdw_cmd_delete(c);
	}
	mutex_unlock(&mpriv->mtx);

	mdw_trace_end();

	return ret;
}
