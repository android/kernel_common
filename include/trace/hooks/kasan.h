/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM kasan

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_KASAN_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_KASAN_H

#include <trace/hooks/vendor_hooks.h>

DECLARE_HOOK(android_vh_poison_kmalloc_large_redzone,
	TP_PROTO(const void *ptr, size_t size, unsigned long *end),
	TP_ARGS(ptr, size, end));

#endif /* _TRACE_HOOK_KASAN_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
