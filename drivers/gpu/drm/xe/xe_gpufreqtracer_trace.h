/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2025 Intel Corporation
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM power

#if !defined(_XE_GPUFREQTRACER_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _XE_GPUFREQTRACER_TRACE_H

#include <linux/tracepoint.h>

/*
 * Tracepoint for GPU frequency changes
 * This tracepoint is exposed at /sys/kernel/debug/tracing/events/power/gpu_frequency
 *
 * location: /d/events/power/gpu_frequency
 * format: {unsigned int state, unsigned int gpu_id}
 * where state holds the frequency(in Khz) and the gpu_id holds the GPU clock domain.
 */

TRACE_EVENT(gpu_frequency,
	TP_PROTO(unsigned int state, unsigned int gpu_id),

	TP_ARGS(state, gpu_id),

	TP_STRUCT__entry(
		__field(unsigned int, state)
		__field(unsigned int, gpu_id)
	),

	TP_fast_assign(
		__entry->state = state;
		__entry->gpu_id = gpu_id;
	),

	TP_printk("state=%u gpu_id=%u", __entry->state, __entry->gpu_id)
);

#endif /* _XE_GPUFREQTRACER_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE xe_gpufreqtracer_trace
#include <trace/define_trace.h>
