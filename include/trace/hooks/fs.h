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

#endif /* _TRACE_HOOK_FS_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
