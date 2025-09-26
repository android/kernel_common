// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_gpufreqtracer.h"

#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/container_of.h>
#include <linux/gfp.h>
#include <linux/atomic.h>
#include <drm/drm_managed.h>
#include <drm/drm_drv.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_gt_types.h"
#include "xe_guc_pc.h"
#include "xe_module.h"
#include "xe_force_wake.h"
#include "xe_pm.h"

/* Job submission frequency monitoring constants */
#define XE_GPUFREQ_SUBMISSION_WINDOW_MS		1000	/* 1 second window */
#define XE_GPUFREQ_HIGH_FREQ_THRESHOLD		5	/* submissions per window for high freq */
#define XE_GPUFREQ_IDLE_TIMEOUT_MS		2000	/* timeout for switching back to normal */
#define XE_GPUFREQ_DECAY_FACTOR			2	/* decay factor for submission rate */
#define XE_GPUFREQ_MAX_DECAY_PERIODS		32	/* max safe bit shift periods for int */

#define CREATE_TRACE_POINTS
#include "xe_gpufreqtracer_trace.h"

/* forward declarations */
struct xe_gpufreqtracer_gt_data;
static int xe_gpufreqtracer_start_monitoring(struct xe_gt *gt);
static void xe_gpufreqtracer_sample_work(struct work_struct *work);
static void xe_gpufreqtracer_cleanup_action(struct drm_device *drm, void *ptr);
static void xe_gpufreqtracer_check_idle_timeout(struct xe_gpufreqtracer_gt_data *gt_data);
static void xe_gpufreqtracer_apply_decay(struct xe_gpufreqtracer_gt_data *gt_data,
					 unsigned long now);
static void xe_gpufreqtracer_adjust_frequency(struct xe_gt *gt,
					      struct xe_gpufreqtracer_gt_data *gt_data,
					      int need_fast_poll, int current_rate);
static bool xe_gpufreqtracer_prepare_device_access(struct xe_gpufreqtracer_gt_data *gt_data,
						   int *drm_idx, unsigned int *fw_ref);

/**
 * struct xe_gpufreqtracer_gt_data - Per-GT frequency monitoring data
 * @gt: Reference to the GT
 * @delayed_work: Delayed work for periodic monitoring
 * @last_frequency: Last reported frequency to avoid duplicate reports
 * @monitoring_active: Whether monitoring is currently active
 * @gpu_active: Overall GPU activity state
 * @submission_rate: Decaying submission rate counter
 * @last_submission_time: Timestamp of last job submission
 * @last_decay_time: Last time decay was applied
 */
struct xe_gpufreqtracer_gt_data {
	struct xe_gt *gt;
	struct delayed_work delayed_work;
	u32 last_frequency;
	atomic_t monitoring_active;
	atomic_t gpu_active;
	atomic_t submission_rate;
	unsigned long last_submission_time;
	unsigned long last_decay_time;
};

/**
 * struct xe_gpufreqtracer_data - Per-device frequency tracer data
 * @xe: Reference to the XE device
 * @gt_data: Array of per-GT monitoring data
 */
struct xe_gpufreqtracer_data {
	struct xe_device *xe;
	struct xe_gpufreqtracer_gt_data *gt_data;
};

/**
 * xe_gpufreqtracer_validate_params - Validate GPU frequency monitoring parameters
 *
 * Validates and corrects the GPU frequency monitoring interval parameter.
 * If the parameter is out of range, it will be reset to the default value.
 */
static void xe_gpufreqtracer_validate_params(void)
{
	if (xe_modparam.gpufreq_monitoring_interval_ms < XE_GPUFREQ_MONITORING_MIN_INTERVAL_MS ||
	    xe_modparam.gpufreq_monitoring_interval_ms > XE_GPUFREQ_MONITORING_MAX_INTERVAL_MS) {
		pr_warn("xe: gpufreq_monitoring_interval_ms %u out of range [%u, %u], using default %u ms\n",
			xe_modparam.gpufreq_monitoring_interval_ms,
			XE_GPUFREQ_MONITORING_MIN_INTERVAL_MS,
			XE_GPUFREQ_MONITORING_MAX_INTERVAL_MS,
			XE_GPUFREQ_MONITORING_DEFAULT_INTERVAL_MS);
		xe_modparam.gpufreq_monitoring_interval_ms =
			XE_GPUFREQ_MONITORING_DEFAULT_INTERVAL_MS;
	}
}

/**
 * xe_gpufreqtracer_init - Initialize GPU frequency tracer for a device
 * @xe: The XE device
 *
 * Sets up the frequency tracer infrastructure for all GTs in the device.
 *
 * Return: 0 on success, negative error code on failure
 */
int xe_gpufreqtracer_init(struct xe_device *xe)
{
	struct xe_gpufreqtracer_data *tracer_data;
	struct xe_gt *gt;
	u8 tile_id;
	int ret = 0;

	/* Validate module parameters first */
	xe_gpufreqtracer_validate_params();

	tracer_data = drmm_kzalloc(&xe->drm, sizeof(*tracer_data), GFP_KERNEL);
	if (!tracer_data)
		return -ENOMEM;

	tracer_data->xe = xe;

	/* Allocate GT data array based on actual GT count */
	tracer_data->gt_data = drmm_kcalloc(&xe->drm, xe->info.gt_count,
					   sizeof(*tracer_data->gt_data),
					   GFP_KERNEL);
	if (!tracer_data->gt_data) {
		ret = -ENOMEM;
		goto err_free_tracer;
	}

	/* Initialize per-GT data */
	for_each_gt(gt, xe, tile_id) {
		struct xe_gpufreqtracer_gt_data *gt_data =
			&tracer_data->gt_data[gt->info.id];

		drm_dbg(&xe->drm, "initializing GT%u (tile %u)", gt->info.id, tile_id);

		gt_data->gt = gt;
		atomic_set(&gt_data->monitoring_active, 0);
		atomic_set(&gt_data->gpu_active, 0);
		atomic_set(&gt_data->submission_rate, 0);
		gt_data->last_frequency = 0;
		gt_data->last_submission_time = jiffies;
		gt_data->last_decay_time = jiffies;

		INIT_DELAYED_WORK(&gt_data->delayed_work, xe_gpufreqtracer_sample_work);

		drm_dbg(&xe->drm, "GT%u initialized with global interval=%u ms",
			 gt->info.id, xe_modparam.gpufreq_monitoring_interval_ms);
	}

	xe->gpufreqtracer_data = tracer_data;

	/* Start periodic monitoring on all GTs using global module parameter */
	for_each_gt(gt, xe, tile_id) {
		ret = xe_gpufreqtracer_start_monitoring(gt);
		if (ret) {
			drm_err(&xe->drm, "xe_gpufreqtracer: failed to start monitoring for GT%u, err=%d\n",
				gt->info.id, ret);
		}
	}

	/* Register cleanup action for proper work cancellation */
	ret = drmm_add_action(&xe->drm, xe_gpufreqtracer_cleanup_action, xe);
	if (ret)
		return ret;

	return 0;

err_free_tracer:
	drm_err(&xe->drm, "initialization failed, freeing tracer data");
	return ret;
}

/**
 * xe_gpufreqtracer_sample_work - Worker function to sample GPU frequency.
 * @work: Pointer to the delayed_work_struct representing the scheduled work.
 *
 * This function is executed in a workqueue context to periodically sample
 * the GPU frequency and perform any necessary tracing or logging operations.
 * It reschedules itself for the next sampling interval.
 *
 * The function includes proper power management and hotplug protection:
 * - Uses drm_dev_enter/exit to protect against device removal
 * - Uses xe_pm_runtime_get_if_active to avoid waking suspended devices
 * - Uses xe_force_wake_get to ensure GT domain is powered for MMIO reads
 */
static void xe_gpufreqtracer_sample_work(struct work_struct *work)
{
	struct xe_gpufreqtracer_gt_data *gt_data =
		container_of(work, struct xe_gpufreqtracer_gt_data, delayed_work.work);
	struct xe_gt *gt = gt_data->gt;
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_guc_pc *pc = &gt->uc.guc.pc;
	u32 current_freq, last_freq;
	unsigned int fw_ref;
	int drm_idx;

	if (!atomic_read(&gt_data->monitoring_active)) {
		drm_warn(&xe->drm, "monitoring not active for GT%u, exiting",
			 gt->info.id);
		return;
	}

	if (!xe_gpufreqtracer_prepare_device_access(gt_data, &drm_idx, &fw_ref))
		goto schedule_next;

	current_freq = xe_guc_pc_get_act_freq(pc) * 1000; /* Convert MHz to KHz */
	last_freq = gt_data->last_frequency;

	/* Only report if frequency has changed or this is the first sample */
	if (current_freq != last_freq) {
#if IS_ENABLED(CONFIG_DRM_XE_DEBUG)
		drm_dbg(&xe->drm, "GT%u frequency changed, tracing %u KHz (period %u msec)",
			gt->info.id, current_freq,
			atomic_read(&gt_data->gpu_active)
				? XE_GPUFREQ_MONITORING_MIN_INTERVAL_MS
				: xe_modparam.gpufreq_monitoring_interval_ms);
#endif
		trace_gpu_frequency(current_freq, gt->info.id);
		gt_data->last_frequency = current_freq;
	}

	xe_force_wake_put(gt_to_fw(gt), fw_ref);
	xe_pm_runtime_put(xe);
	drm_dev_exit(drm_idx);

schedule_next:
	xe_gpufreqtracer_check_idle_timeout(gt_data);

	/* Reschedule for the next appropriate interval */
	unsigned long delay = atomic_read(&gt_data->gpu_active)
		? XE_GPUFREQ_MONITORING_MIN_INTERVAL_MS
		: xe_modparam.gpufreq_monitoring_interval_ms;
	schedule_delayed_work(&gt_data->delayed_work, msecs_to_jiffies(delay));
}

/**
 * xe_gpufreqtracer_track_submission - Track job submission frequency and adjust monitoring
 * @gt: The GT instance
 *
 * Tracks the frequency of job submissions using an exponential decay mechanism and
 * timeout-based fallback.
 */
void xe_gpufreqtracer_track_submission(struct xe_gt *gt)
{
	struct xe_gpufreqtracer_data *tracer_data = gt_to_xe(gt)->gpufreqtracer_data;
	struct xe_gpufreqtracer_gt_data *gt_data;
	unsigned long now = jiffies;
	int current_rate;
	int need_fast_poll;

	if (!tracer_data || gt->info.id >= gt_to_xe(gt)->info.gt_count)
		return;

	gt_data = &tracer_data->gt_data[gt->info.id];
	if (!atomic_read(&gt_data->monitoring_active))
		return;

	xe_gpufreqtracer_apply_decay(gt_data, now);

	/* Increment submission rate */
	current_rate = atomic_add_return(1, &gt_data->submission_rate);
	gt_data->last_submission_time = now;

	/* Decide if high frequency monitoring is needed */
	need_fast_poll = (current_rate >= XE_GPUFREQ_HIGH_FREQ_THRESHOLD) ? 1 : 0;

	xe_gpufreqtracer_adjust_frequency(gt, gt_data, need_fast_poll, current_rate);
}

/**
 * xe_gpufreqtracer_start_monitoring - Start periodic frequency monitoring
 * @gt: The GT instance
 *
 * Starts periodic sampling of GPU frequency for the specified GT using the global
 * monitoring interval from module parameters.
 *
 * Return: 0 on success, negative error code on failure
 */
static int xe_gpufreqtracer_start_monitoring(struct xe_gt *gt)
{
	struct xe_gpufreqtracer_data *tracer_data = gt_to_xe(gt)->gpufreqtracer_data;
	struct xe_gpufreqtracer_gt_data *gt_data;

	if (!tracer_data) {
		drm_warn(&gt_to_xe(gt)->drm, "no tracer data for GT%u, not supported", gt->info.id);
		return -EOPNOTSUPP;
	}

	if (gt->info.id >= gt_to_xe(gt)->info.gt_count) {
		drm_err(&gt_to_xe(gt)->drm, "invalid GT ID %u, max supported is %u",
			gt->info.id, gt_to_xe(gt)->info.gt_count - 1);
		return -EINVAL;
	}

	gt_data = &tracer_data->gt_data[gt->info.id];

	if (atomic_read(&gt_data->monitoring_active)) {
		drm_warn(&gt_to_xe(gt)->drm, "monitoring already active for GT%u", gt->info.id);
		return -EALREADY;
	}

	atomic_set(&gt_data->monitoring_active, 1);
	atomic_set(&gt_data->gpu_active, 0);
	atomic_set(&gt_data->submission_rate, 0);
	gt_data->last_frequency = 0;
	gt_data->last_submission_time = jiffies;
	gt_data->last_decay_time = jiffies;

	/* Start the delayed work using global interval */
	schedule_delayed_work(&gt_data->delayed_work,
				  msecs_to_jiffies(xe_modparam.gpufreq_monitoring_interval_ms));

	drm_dbg(&gt_to_xe(gt)->drm, "monitoring started for GT%u with interval %u ms",
		 gt->info.id, xe_modparam.gpufreq_monitoring_interval_ms);

	return 0;
}

/**
 * xe_gpufreqtracer_stop_monitoring - Stop periodic frequency monitoring
 * @gt: The GT instance
 *
 * Stops periodic sampling of GPU frequency for the specified GT.
 */
static void xe_gpufreqtracer_stop_monitoring(struct xe_gt *gt)
{
	struct xe_gpufreqtracer_data *tracer_data = gt_to_xe(gt)->gpufreqtracer_data;
	struct xe_gpufreqtracer_gt_data *gt_data;

	if (!tracer_data || gt->info.id >= gt_to_xe(gt)->info.gt_count) {
		drm_err(&gt_to_xe(gt)->drm, "invalid tracer data or GT ID %u for stop request",
			gt->info.id);
		return;
	}

	gt_data = &tracer_data->gt_data[gt->info.id];

	if (!atomic_read(&gt_data->monitoring_active)) {
		drm_warn(&gt_to_xe(gt)->drm, "monitoring not active for GT%u, nothing to stop",
			 gt->info.id);
		return;
	}

	atomic_set(&gt_data->monitoring_active, 0);
	atomic_set(&gt_data->gpu_active, 0);
	atomic_set(&gt_data->submission_rate, 0);

	cancel_delayed_work_sync(&gt_data->delayed_work);
}

/**
 * xe_gpufreqtracer_apply_decay - Apply exponential decay to submission rate
 * @gt_data: Per-GT monitoring data
 * @now: Current time in jiffies
 *
 * Applies exponential decay to the submission rate based on time elapsed
 * since the last decay operation. The rate is halved for each second of inactivity.
 */
static void xe_gpufreqtracer_apply_decay(struct xe_gpufreqtracer_gt_data *gt_data,
					 unsigned long now)
{
	unsigned long time_since_last_decay;
	int current_rate, decay_periods;

	time_since_last_decay = now - gt_data->last_decay_time;
	if (time_since_last_decay >= msecs_to_jiffies(XE_GPUFREQ_SUBMISSION_WINDOW_MS)) {
		/* Calculate how many decay periods have passed (each period = 1 second) */
		decay_periods = time_since_last_decay /
				msecs_to_jiffies(XE_GPUFREQ_SUBMISSION_WINDOW_MS);

		current_rate = atomic_read(&gt_data->submission_rate);
		if (current_rate > 0 && decay_periods > 0) {
			/* Apply exponential decay: rate = rate / (2^decay_periods)
			 * If too many periods passed, just reset to zero to avoid underflow
			 */
			if (decay_periods >= XE_GPUFREQ_MAX_DECAY_PERIODS)
				current_rate = 0;
			else
				current_rate >>= decay_periods; /* rate / (2^periods) */

			atomic_set(&gt_data->submission_rate, current_rate);
		}
		gt_data->last_decay_time = now;
	}
}

/**
 * xe_gpufreqtracer_check_idle_timeout - Check for idle timeout and switch to normal polling
 * @gt_data: Per-GT monitoring data
 *
 * Checks if the GPU has been idle for too long and switches back to normal
 * polling frequency if the idle timeout has been exceeded.
 */
static void xe_gpufreqtracer_check_idle_timeout(struct xe_gpufreqtracer_gt_data *gt_data)
{
	struct xe_gt *gt = gt_data->gt;
	struct xe_device *xe = gt_to_xe(gt);

	if (atomic_read(&gt_data->gpu_active)) {
		/* Calculate how long GPU has been idle since last submission */
		unsigned long idle_time = jiffies - gt_data->last_submission_time;

		/* Check if idle timeout (2s) has been exceeded - fallback to normal polling */
		if (time_after(jiffies, gt_data->last_submission_time +
			       msecs_to_jiffies(XE_GPUFREQ_IDLE_TIMEOUT_MS))) {
			/* Switch back to normal frequency monitoring and reset counters */
			atomic_set(&gt_data->gpu_active, 0);
			atomic_set(&gt_data->submission_rate, 0);
			drm_dbg(&xe->drm, "GT%u timed out from high frequency polling after %u ms idle",
				gt->info.id, jiffies_to_msecs(idle_time));
		}
	}
}

/**
 * xe_gpufreqtracer_adjust_frequency - Adjust monitoring frequency based on activity
 * @gt: The GT instance
 * @gt_data: Per-GT monitoring data
 * @need_fast_poll: Whether high frequency monitoring is needed
 * @current_rate: Current submission rate for logging
 *
 * Switches between high frequency and normal frequency monitoring based on
 * the current GPU activity level.
 */
static void xe_gpufreqtracer_adjust_frequency(struct xe_gt *gt,
					      struct xe_gpufreqtracer_gt_data *gt_data,
					      int need_fast_poll, int current_rate)
{
	if (need_fast_poll && !atomic_read(&gt_data->gpu_active)) {
		/* Switch to high frequency polling */
		atomic_set(&gt_data->gpu_active, 1);
		mod_delayed_work(system_wq, &gt_data->delayed_work,
				msecs_to_jiffies(XE_GPUFREQ_MONITORING_MIN_INTERVAL_MS));
		drm_dbg(&gt_to_xe(gt)->drm,
			"GT%u switched to high frequency polling (rate=%d)",
			gt->info.id, current_rate);
	} else if (!need_fast_poll && atomic_read(&gt_data->gpu_active)) {
		/* Switch back to normal frequency polling */
		atomic_set(&gt_data->gpu_active, 0);
		mod_delayed_work(system_wq, &gt_data->delayed_work,
				msecs_to_jiffies(xe_modparam.gpufreq_monitoring_interval_ms));
		drm_dbg(&gt_to_xe(gt)->drm,
			"GT%u switched to normal frequency polling (rate=%d)",
			gt->info.id, current_rate);
	}
}

/**
 * xe_gpufreqtracer_prepare_device_access - Prepare device for safe frequency access
 * @gt_data: Per-GT monitoring data
 * @drm_idx: Pointer to store DRM device index for cleanup
 * @fw_ref: Pointer to store forcewake reference for cleanup
 *
 * Performs all necessary device safety checks and power management setup
 * before accessing GPU frequency registers. This includes device hotplug
 * protection, runtime PM management, and forcewake acquisition.
 *
 * Return: true if device is ready for access, false otherwise
 */
static bool xe_gpufreqtracer_prepare_device_access(struct xe_gpufreqtracer_gt_data *gt_data,
						   int *drm_idx, unsigned int *fw_ref)
{
	struct xe_gt *gt = gt_data->gt;
	struct xe_device *xe = gt_to_xe(gt);

	/* Protect against device hotplug/removal */
	if (!drm_dev_enter(&xe->drm, drm_idx)) {
		drm_err(&xe->drm, "device unplugged, stopping monitoring for GT%u",
			gt->info.id);
		atomic_set(&gt_data->monitoring_active, 0);
		return false;
	}

	/* Get runtime PM reference only if device is already active - don't wake it */
	if (!xe_pm_runtime_get_if_active(xe)) {
		drm_warn(&xe->drm, "device not active, skipping frequency read for GT%u",
			gt->info.id);
		drm_dev_exit(*drm_idx);
		return false;
	}

	/* Get forcewake to ensure GT domain is powered */
	*fw_ref = xe_force_wake_get(gt_to_fw(gt), XE_FW_GT);
	if (*fw_ref < 0) {
		drm_warn(&xe->drm, "failed to get forcewake all, for GT%u, skipping sample",
			 gt->info.id);
		xe_pm_runtime_put(xe);
		drm_dev_exit(*drm_idx);
		return false;
	}

	return true;
}

/**
 * xe_gpufreqtracer_cleanup_action - DRM managed cleanup action
 * @drm: DRM device
 * @ptr: Pointer to xe_device
 *
 * Cleanup function called automatically by DRM managed resource system.
 */
static void xe_gpufreqtracer_cleanup_action(struct drm_device *drm, void *ptr)
{
	struct xe_device *xe = ptr;
	struct xe_gpufreqtracer_data *tracer_data = xe->gpufreqtracer_data;
	struct xe_gt *gt;
	u8 tile_id;

	if (!tracer_data) {
		drm_warn(drm, "no tracer data found, nothing to cleanup");
		return;
	}

	/* Stop all monitoring */
	for_each_gt(gt, xe, tile_id) {
		drm_dbg(drm, "stopping monitoring for GT%u", gt->info.id);
		xe_gpufreqtracer_stop_monitoring(gt);
	}

	/* Memory is automatically freed by drmm - just clear the pointer */
	xe->gpufreqtracer_data = NULL;
}
