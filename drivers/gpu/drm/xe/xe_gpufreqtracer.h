/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_GPUFREQTRACER_H_
#define _XE_GPUFREQTRACER_H_

#include <linux/types.h>

/* GPU frequency monitoring interval constants (in milliseconds) */
#define XE_GPUFREQ_MONITORING_MIN_INTERVAL_MS	100
#define XE_GPUFREQ_MONITORING_MAX_INTERVAL_MS	10000
#define XE_GPUFREQ_MONITORING_DEFAULT_INTERVAL_MS	5000

struct xe_device;
struct xe_gt;

#ifdef CONFIG_DRM_XE_GPUFREQTRACER

/*
 * Initialize the GPU frequency tracer for a device
 */
int xe_gpufreqtracer_init(struct xe_device *xe);

void xe_gpufreqtracer_track_submission(struct xe_gt *gt);

#else /* CONFIG_DRM_XE_GPUFREQTRACER */

static inline int xe_gpufreqtracer_init(struct xe_device *xe)
{
	return 0;
}

static inline void xe_gpufreqtracer_track_submission(struct xe_gt *gt) {}

#endif /* CONFIG_DRM_XE_GPUFREQTRACER */

#endif /* _XE_GPUFREQTRACER_H_ */
