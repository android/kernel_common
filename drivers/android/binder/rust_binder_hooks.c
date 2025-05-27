// SPDX-License-Identifier: GPL-2.0-only
/* rust_binder_events.c
 *
 * Rust Binder vendorhooks.
 *
 * Copyright 2024 Google LLC
 */

#include "rust_binder.h"

#define CREATE_TRACE_POINTS
#define CREATE_RUST_TRACE_POINTS
#include <trace/hooks/vendor_hooks.h>
#include <linux/tracepoint.h>

#include "rust_binder_hooks.h"

/*
 * Export tracepoints that act as a bare tracehook (ie: have no trace event
 * associated with them) to allow external modules to probe them.
 */
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_rust_binder_set_priority);
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_rust_binder_restore_priority);
