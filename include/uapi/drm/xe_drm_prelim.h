/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __XE_DRM_PRELIM_H__
#define __XE_DRM_PRELIM_H__

#include "xe_drm.h"

/*
 * Modifications to structs/values defined here are subject to
 * backwards-compatibility constraints.
 *
 * Internal/downstream declarations must be added here, not to
 * xe_drm.h. The values in xe_drm_prelim.h must also be kept
 * synchronized with values in xe_drm.h.
 */

/* PRELIM ioctl numbers go down from 0x5f */
#define DRM_XE_PRELIM_PXP_OPS		0x52
/* NOTE: PXP_OPS PRELIM ioctl code 0x52 maintains compatibility with DII-server products */

#define DRM_IOCTL_XE_PRELIM_PXP_OPS	DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_PRELIM_PXP_OPS, \
						 struct drm_xe_prelim_pxp_ops)

/* End PRELIM ioctl's */

/*
 * struct drm_xe_prelim_pxp_query_host_session_handle
 * Contains params to get a host-session-handle that the user-space
 * process uses for all communication with the GSC-FW.
 *
 * - Each user space process is provided a single host_session_handle.
 *   A user space process that repeats a request for a host_session_handle
 *   will be successfully serviced but returned the same host_session_handle
 *   that was generated (a random number) on the first request.
 * - When the user space process exits, the kernel driver will send a cleanup
 *   cmd to the gsc firmware. There is no need (and no mechanism) for the user
 *   space process to explicitly request to release its host_session_handle.
 * - The host_session_handle remains valid through any suspend/resume cycles
 *   and through PXP hw-session-slot teardowns (essentially they are
 *   decoupled from the hw session-slots)
 *
 * This operation can only fail if something goes wrong in the prep steps, in
 * which case the ioctl will fail. Therefore, if the ioctl succedes the
 * pxp_ops.status will always be DRM_XE_PRELIM_PXP_OP_STATUS_SUCCESS
 */
struct drm_xe_prelim_pxp_query_host_session_handle {
	__u64 host_session_handle; /* out - returned host_session_handle */
} __attribute__((packed));

/*
 * drm_xe_prelim_pxp_ops
 *
 * PXP is an Xe componment, that helps user space to establish the hardware
 * protected session and manage the status of each alive software session,
 * as well as the life cycle of each session.
 *
 * This ioctl is to allow user space driver to create, set, and destroy each
 * session. It also provides the communication chanel to TEE (Trusted
 * Execution Environment) for the protected hardware session creation.
 */
struct drm_xe_prelim_pxp_ops {
	/** @extensions: MBZ, as no extension are currently defined for this ioctl */
	__u64 extensions;

	/** @action: operation to perform */
	__u32 action;
#define DRM_XE_PRELIM_PXP_ACTION_HOST_SESSION_HANDLE_REQ 0

	/** @status: returned outcome of the operation */
	__u32 status;
#define DRM_XE_PRELIM_PXP_OP_STATUS_SUCCESS 0

	/*
	 * in/out: action-specific data. Must fill the structure matching the
	 * selected action.
	 */
	union {
		/** @query_handle: parameters for the HOST_SESSION_HANDLE_REQ action */
		struct drm_xe_prelim_pxp_query_host_session_handle query_handle;
	};
} __attribute__((packed));

#endif /* __XE_DRM_PRELIM_H__ */

