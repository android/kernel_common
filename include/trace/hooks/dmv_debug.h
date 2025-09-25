/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dmv_debug

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_DMV_DEBUG_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_DMV_DEBUG_H

#include <trace/hooks/vendor_hooks.h>

DECLARE_HOOK(android_vh_handle_add_fec_mismatch_blks,
		TP_PROTO(sector_t cur_blk, char *dev_name),
		TP_ARGS(cur_blk, dev_name));

struct dm_verity;
struct dm_verity_io;

DECLARE_HOOK(android_vh_handle_data_error,
		TP_PROTO(struct dm_verity *v,
			sector_t block, struct dm_verity_io *io,
			const u8 *data, const u8 *want_digest),
		TP_ARGS(v, block, io, data, want_digest));

DECLARE_HOOK(android_vh_handle_metadata_error,
		TP_PROTO(struct dm_verity *v,
			sector_t block, struct dm_verity_io *io,
			const u8 *want_digest),
		TP_ARGS(v, block, io, want_digest));

DECLARE_HOOK(android_vh_handle_add_skipped_blks,
		TP_PROTO(void *unused),
		TP_ARGS(unused));

DECLARE_HOOK(android_vh_handle_add_blks_map,
		TP_PROTO(long long val, char *dev_name),
		TP_ARGS(val, dev_name));

DECLARE_HOOK(android_vh_handle_get_b_info,
		TP_PROTO(char *dev_name),
		TP_ARGS(dev_name));

#endif /* _TRACE_HOOK_DMV_DEBUG_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
