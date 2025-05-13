/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ogki_honor
#ifdef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_PATH
#endif
#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_OGKI_HONOR_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_OGKI_HONOR_H
#include <trace/hooks/vendor_hooks.h>

struct task_struct;
DECLARE_HOOK(android_vh_ogki_check_vip_status,
	TP_PROTO(int cur_pid, int cur_tgid, struct task_struct *task, int *ret),
	TP_ARGS(cur_pid, cur_tgid, task, ret));
DECLARE_RESTRICTED_HOOK(android_rvh_ogki_task_util,
	TP_PROTO(struct task_struct *p, unsigned long *ret),
	TP_ARGS(p, ret), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_ogki_uclamp_task_util,
	TP_PROTO(struct task_struct *p, unsigned long *ret),
	TP_ARGS(p, ret), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_ogki_get_task_tags,
	TP_PROTO(struct task_struct *p, unsigned long long *ret),
	TP_ARGS(p, ret), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_ogki_get_task_rsum,
	TP_PROTO(struct task_struct *p, unsigned long long *ret),
	TP_ARGS(p, ret), 1);
#endif /* _TRACE_HOOK_OGKI_ogki_H */
/* This part must be outside protection */
#include <trace/define_trace.h>

