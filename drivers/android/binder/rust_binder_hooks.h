/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Google, Inc.
 */

#undef TRACE_SYSTEM
#undef TRACE_INCLUDE_FILE
#undef TRACE_INCLUDE_PATH
#define TRACE_SYSTEM rust_binder
#define TRACE_INCLUDE_FILE rust_binder_hooks
#define TRACE_INCLUDE_PATH ../drivers/android/binder

#if !defined(_RUST_BINDER_HOOK_H) || defined(TRACE_HEADER_MULTI_READ)
#define _RUST_BINDER_HOOK_H

#include <trace/hooks/vendor_hooks.h>

/*
 * Following tracepoints are not exported in tracefs and provide a
 * mechanism for vendor modules to hook and extend functionality
 */

DECLARE_HOOK(android_vh_rust_binder_set_priority,
	TP_PROTO(rust_binder_transaction t, struct task_struct *task),
	TP_ARGS(t, task));
DECLARE_HOOK(android_vh_rust_binder_restore_priority,
	TP_PROTO(struct task_struct *task),
	TP_ARGS(task));

#endif /* _RUST_BINDER_HOOK_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
