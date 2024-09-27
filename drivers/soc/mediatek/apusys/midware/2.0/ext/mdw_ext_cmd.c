// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include "mdw_ext.h"
#include "mdw_ext_cmn.h"
#include "mdw_ext_ioctl.h"
#include "mdw_ext_export.h"
#include "mdw_cmd.h"

#define MDWEXT_IDKID2EXTID(id, kid) (((0xffffffff & (uint64_t)(task_tgid_nr(current))) << 32) \
	| ((0xffff & kid) << 16) | (0xffff & id))
#define MDWEXT_EXTID2TGID(extid) (extid >> 32)
#define MDWEXT_EXTID2KID(extid) (0xffff & (extid >> 16))
#define MDWEXT_EXTID2ID(extid) (0xffff & extid)
#define MDWEXT_KID16BIT(kid) (0xffff & kid)

static inline int mdw_ext_cmd_sanity_check(struct mdw_cmd *c)
{
	if (MDWEXT_EXTID2KID(c->ext_id) != MDWEXT_KID16BIT(c->kid) ||
		MDWEXT_EXTID2ID(c->ext_id) < MDW_EXT_IDR_MIN ||
		MDWEXT_EXTID2ID(c->ext_id) > MDW_EXT_IDR_MAX)
		return -EINVAL;

	return 0;
}

void mdw_ext_cmd_get_id(struct mdw_cmd *c)
{
	int ret = 0;
	char comm[16];

	mdw_ext_lock();
	if (c->ext_id) {
		mdwext_drv_warn("c(0x%llx) ext_id(0x%llx) already exist\n", c->kid, c->ext_id);
		goto out;
	}

	/* alloc idr */
	ret = idr_alloc_cyclic(&mdw_ext_dev->ext_ids, c, MDW_EXT_IDR_MIN, MDW_EXT_IDR_MAX, GFP_KERNEL);
	if (ret < MDW_EXT_IDR_MIN) {
		mdwext_drv_warn("driver alloc ext id fail(%d/%d~%d)\n", ret, MDW_EXT_IDR_MIN, MDW_EXT_IDR_MAX);
	} else {
		/* get id success, gen extid */
		c->ext_id = MDWEXT_IDKID2EXTID(ret, c->kid);
		mdwext_cmd_debug("c(0x%llx) extid(0x%llx) tgid(%d:%s/%d:%s)\n",
			c->kid, c->ext_id, task_tgid_nr(current), comm, c->tgid, c->comm);
	}

out:
	mdw_ext_unlock();
}

void mdw_ext_cmd_put_id(struct mdw_cmd *c)
{
	if (c->mpriv->mdev->mdw_ver < 4)
		return;

	mdwext_cmd_debug("c(0x%llx) extid(0x%llx) tgid(%d/%d)\n",
		c->kid, c->ext_id, task_tgid_nr(current), c->tgid);

	/* don't need lock ext lock because mdw_cmd_put() already get mutex */
	if (mdw_ext_cmd_sanity_check(c)) {
		mdwext_drv_warn("c(0x%llx) extid(0x%llx) not available, cmd tgid(%d)\n",
			c->kid, c->ext_id, c->tgid);
	} else {
		idr_remove(&mdw_ext_dev->ext_ids, MDWEXT_EXTID2ID(c->ext_id));
		c->ext_id = 0;
	}
}

static int mdw_ext_cmd_ioctl_run(union mdw_ext_cmd_args *args)
{
	struct mdw_cmd *c = NULL;
	union mdw_cmd_args c_args;
	int ret = -EINVAL;

	mdw_ext_lock();
	/* find cmd */
	c = (struct mdw_cmd *)idr_find(&mdw_ext_dev->ext_ids, MDWEXT_EXTID2ID(args->in.ext_id));
	if (!c) {
		mdwext_drv_err("invalid extid(0x%llx)\n", args->in.ext_id);
		goto unlock_extlock;
	}

	/* check support */
	if (c->mpriv->mdev->mdw_ver < 4) {
		mdwext_drv_warn("no support mdw ext\n");
		goto unlock_extlock;
	}
	/* check cmd */
	if (c->ext_id != args->in.ext_id) {
		mdwext_drv_err("c(0x%llx) extid not available(0x%llx/0x%llx)\n",
			c->kid, args->in.ext_id, c->ext_id);
		goto unlock_extlock;
	}
	if (c->u_pid != task_tgid_nr(current) || !c->u_pid) {
		mdwext_drv_err("c(0x%llx) pid not match(0x%x/0x%x)\n",
			c->kid, c->u_pid, task_tgid_nr(current));
		goto unlock_extlock;
	}
	if (mdw_ext_cmd_sanity_check(c)) {
		mdwext_drv_err("c(0x%llx) extid(0x%llx) not available, cmd tgid(%d)\n",
			c->kid, c->ext_id, c->tgid);
		goto unlock_extlock;
	}

	/* get cmd refcnt to avoid racing */
	mdw_cmd_get(c);
	mdw_ext_unlock();

	/* trigger cmd */
	memset(&c_args, 0, sizeof(c_args));
	c_args.in.op = MDW_CMD_IOCTL_RUN_STALE;
	c_args.in.id = c->id;
	ret = mdw_cmd_ioctl(c->mpriv, &c_args);
	if (ret) {
		mdwext_drv_err("c(0x%llx) extid(0x%llx) trigger fail(%d)\n", c->kid, c->ext_id, ret);
		mdw_cmd_put(c);
		goto out;
	}

	memset(args, 0, sizeof(*args));
	args->out.fence = c_args.out.exec.fence;
	args->out.u_done = c_args.out.exec.cmd_done_usr;

	mdw_cmd_put(c);
	goto out;

unlock_extlock:
	mdw_ext_unlock();
out:
	return ret;
}

int mdw_ext_cmd_ioctl(void *data)
{
	union mdw_ext_cmd_args *args = (union mdw_ext_cmd_args *)data;
	int ret = 0;
	uint64_t ext_id = args->in.ext_id;

	mdwext_cmd_debug("extid(0x%llx)\n", ext_id);
	ret = mdw_ext_cmd_ioctl_run(args);
	mdwext_drv_debug("extid(0x%llx) trigger done, fence(%llu) udone(%llu) ret(%d)\n",
		ext_id, args->out.fence, args->out.u_done, ret);

	return ret;
}
