/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM sys
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_SYS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_SYS_H
#include <trace/hooks/vendor_hooks.h>

struct task_struct;
DECLARE_HOOK(android_vh_syscall_prctl_finished,
	TP_PROTO(int option, struct task_struct *task),
	TP_ARGS(option, task));
DECLARE_HOOK(android_vh_security_audit_log_setid,
	TP_PROTO(u32 type, u32 old_id, u32 new_id),
	TP_ARGS(type, old_id, new_id));
#endif

#include <trace/define_trace.h>
