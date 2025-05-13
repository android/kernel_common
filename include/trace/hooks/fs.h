/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fs

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_FS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_FS_H

#include <trace/hooks/vendor_hooks.h>
struct f2fs_sb_info;
struct va_format;

DECLARE_RESTRICTED_HOOK(android_rvh_f2fs_down_read,
	TP_PROTO(wait_queue_head_t *read_waiters, struct rw_semaphore *rwsem, bool *skip),
	TP_ARGS(read_waiters, rwsem, skip), 1);

DECLARE_HOOK(android_vh_f2fs_improve_priority,
	TP_PROTO(struct task_struct *p, int *saved_prio, bool *skip),
	TP_ARGS(p, saved_prio, skip));

DECLARE_HOOK(android_vh_f2fs_restore_priority,
	TP_PROTO(struct task_struct *p, int saved_prio),
	TP_ARGS(p, saved_prio));

DECLARE_HOOK(android_vh_f2fs_printk,
	TP_PROTO(struct f2fs_sb_info *sbi, struct va_format *vaf, int level, bool limit_rate),
	TP_ARGS(sbi, vaf, level, limit_rate));

DECLARE_HOOK(android_vh_f2fs_create,
	TP_PROTO(struct inode *inode, struct dentry *dentry),
	TP_ARGS(inode, dentry));

DECLARE_HOOK(android_vh_wb_dirty_limits,
	TP_PROTO(unsigned long *thresh, struct bdi_writeback *wb),
	TP_ARGS(thresh, wb));

DECLARE_HOOK(android_vh_evict,
	TP_PROTO(struct inode *inode),
	TP_ARGS(inode));

DECLARE_HOOK(android_vh_inode_io_list_del,
	TP_PROTO(struct inode *inode, struct bdi_writeback *wb),
	TP_ARGS(inode, wb));

DECLARE_HOOK(android_vh_redirty_tail_locked,
	TP_PROTO(struct list_head **target_list, struct inode *inode,
		 struct bdi_writeback *wb),
	TP_ARGS(target_list, inode, wb));

DECLARE_HOOK(android_vh_queue_io,
	TP_PROTO(struct bdi_writeback *wb, unsigned int for_kupdate,
		 unsigned long dirtied_before, int *moved),
	TP_ARGS(wb, for_kupdate, dirtied_before, moved));

DECLARE_HOOK(android_vh_mark_inode_dirty,
	TP_PROTO(struct inode *inode, struct bdi_writeback *wb, struct list_head **dirty_list),
	TP_ARGS(inode, wb, dirty_list));

DECLARE_HOOK(android_vh_vfs_fsync_range,
	TP_PROTO(struct inode *inode, unsigned long *cut_off),
	TP_ARGS(inode, cut_off));

DECLARE_RESTRICTED_HOOK(android_rvh_do_fcntl,
	TP_PROTO(struct file *filp, unsigned int cmd, unsigned long arg, long *err),
	TP_ARGS(filp, cmd, arg, err), 1);

DECLARE_HOOK(android_vh_f2fs_file_open,
	TP_PROTO(struct inode *inode, struct file *filp),
	TP_ARGS(inode, filp));

#endif /* _TRACE_HOOK_FS_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
