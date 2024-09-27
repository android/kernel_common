/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef __MTK_APUCMD_H__
#define __MTK_APUCMD_H__

#include <linux/math64.h>

#include "mdw_cmn.h"

/* cmd idr range */
#define MDW_CMD_IDR_MIN (1)
#define MDW_CMD_IDR_MAX (64)

/* stale cmd timeout */
#define MDW_STALE_CMD_TIMEOUT (5*1000) //ms

/* poll cmd */
#define MDW_POLL_TIMEOUT (4*1000) //us
#define MDW_POLLTIME_SLEEP_TH(x) div_u64((x) * 65, 100) //us

/* cmd deubg */
#define mdw_cmd_show(c, f) \
	f("cmd(0x%llx/0x%llx/0x%llx/0x%llx/%d/%u)param(%u/%u/%u/%u/"\
	"%u/%u/%u/%u/%u/%llu)subcmds(%u/%pK/%u/%u)pid(%d/%d)(%d)\n", \
	(uint64_t) c->mpriv, c->uid, c->kid, c->rvid, c->id, kref_read(&c->ref), \
	c->priority, c->hardlimit, c->softlimit, \
	c->power_save, c->power_plcy, c->power_dtime, \
	c->app_type, c->inference_ms, c->tolerance_ms, c->is_dtime_set, \
	c->num_subcmds, c->cmdbufs, c->num_cmdbufs, c->size_cmdbufs, \
	c->pid, c->tgid, task_pid_nr(current))

/* cmd ioctl */
int mdw_cmd_ioctl_v2(struct mdw_fpriv *mpriv, void *data);
int mdw_cmd_ioctl_v3(struct mdw_fpriv *mpriv, void *data);
int mdw_cmd_ioctl_v4(struct mdw_fpriv *mpriv, void *data);

/* cmd release */
void mdw_cmd_mpriv_release(struct mdw_fpriv *mpriv);
void mdw_cmd_mpriv_release_without_stale(struct mdw_fpriv *mpriv);

/* cmd history */
void mdw_cmd_history_init(struct mdw_device *mdev);
void mdw_cmd_history_deinit(struct mdw_device *mdev);
struct mdw_cmd_history_tbl *mdw_cmd_ch_tbl_find(struct mdw_cmd *c);
void mdw_cmd_history_reset(struct mdw_fpriv *mpriv);

/* cmdbuf */
void mdw_cmd_cmdbuf_out(struct mdw_fpriv *mpriv, struct mdw_cmd *c);
void mdw_cmd_put_cmdbufs(struct mdw_fpriv *mpriv, struct mdw_cmd *c);
int mdw_cmd_get_cmdbufs(struct mdw_fpriv *mpriv, struct mdw_cmd *c);
int mdw_cmd_get_cmdbufs_with_apummu(struct mdw_fpriv *mpriv, struct mdw_cmd *c);

/* cmd infos */
int mdw_cmd_create_infos(struct mdw_fpriv *mpriv, struct mdw_cmd *c);
void mdw_cmd_delete_infos(struct mdw_fpriv *mpriv, struct mdw_cmd *c);

/* cmd check */
int mdw_cmd_sanity_check(struct mdw_cmd *c);
int mdw_cmd_sc_sanity_check(struct mdw_cmd *c);
int mdw_cmd_link_check(struct mdw_cmd *c);
int mdw_cmd_adj_check(struct mdw_cmd *c);
void mdw_cmd_check_rets(struct mdw_cmd *c, int ret);

/* cmd delete */
void mdw_cmd_delete(struct mdw_cmd *c);
void mdw_cmd_delete_async(struct mdw_cmd *c);
int mdw_cmd_ioctl_del(struct mdw_fpriv *mpriv, union mdw_cmd_args *args);

/* cmd map */
int mdw_cmd_invoke_map(struct mdw_cmd *c, struct mdw_mem_map *map);
void mdw_cmd_unvoke_map(struct mdw_cmd *c);

/* get apummu table */
int mdw_cmd_get_apummutable(struct mdw_fpriv *mpriv, struct mdw_cmd *c);

/* cmd kref */
void mdw_cmd_put(struct mdw_cmd *c);
void mdw_cmd_get(struct mdw_cmd *c);

#endif
