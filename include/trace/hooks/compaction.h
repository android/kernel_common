/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM compaction

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_COMPACTION_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_COMPACTION_H

#include <trace/hooks/vendor_hooks.h>

DECLARE_HOOK(android_vh_proactive_compact_wmark_high,
	TP_PROTO(int *wmark_high),
	TP_ARGS(wmark_high));
#endif /* _TRACE_HOOK_COMPACTION_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
