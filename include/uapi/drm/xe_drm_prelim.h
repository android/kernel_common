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

/**
 * PXP Tag format:
 * bits   0-6: session id
 * bit      7: rsvd
 * bits  8-15: instance id
 * bit     16: session enabled
 * bit     17: mode hm
 * bit     18: rsvd
 * bit     19: mode sm
 * bits 20-31: rsvd
 */
#define DRM_XE_PRELIM_PXP_TAG_SESSION_ID_MASK		(0x7f)
#define DRM_XE_PRELIM_PXP_TAG_INSTANCE_ID_MASK		(0xff << 8)
#define DRM_XE_PRELIM_PXP_TAG_SESSION_ENABLED		(0x1 << 16)
#define DRM_XE_PRELIM_PXP_TAG_SESSION_HM		(0x1 << 17)
#define DRM_XE_PRELIM_PXP_TAG_SESSION_SM		(0x1 << 19)

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
 * struct drm_xe_prelim_pxp_session_op - Params to reserve or release a PXP
 * session. the pxp_tag
 */
struct drm_xe_prelim_pxp_session_op {
	/** @action: session operation to perform (reserve or release). */
	__u32 action;
#define DRM_XE_PRELIM_PXP_SESSION_RESERVE 0
#define DRM_XE_PRELIM_PXP_SESSION_RELEASE 1

	/**
	 * @pxp_tag: when reserving a session, this variable MBZ as input and
	 * will be filled with the pxp_tag as output (see defines in
	 * struct prelim_drm_xe_pxp_query_tag for the format of the tag). When
	 * releasing a session this must be set to either the full tag or
	 * just the ID of the session to be released.
	 */
	__u32 pxp_tag;

	/** @session_type: When reserving a PXP session, specify the of session.
	 * The only supported value is DRM_XE_PXP_TYPE_HWDRM. Ignored when
	 * releasing the session.
	 */
	__u32 session_type;

	/**
	 * @session_mode: When reserving a PXP session, specify the protection
	 * mode. This information is stored in the PXP tag. Ignored when
	 * releasing the session.
	 */
	__u32 session_mode;
#define DRM_XE_PRELIM_PXP_MODE_LM 0
#define DRM_XE_PRELIM_PXP_MODE_HM 1
#define DRM_XE_PRELIM_PXP_MODE_SM 2
} __attribute__((packed));

/*
 * struct drm_xe_pxp_query_tag - Params to query the PXP tag of specified
 * session id and whether the session is alive from PXP state machine.
 */
struct drm_xe_prelim_pxp_query_tag {
	/**
	 * @pxp_tag: as input, this variable must be set to either the pxp_tag
	 * returned by the session reservation or to the session id. The value
	 * will be overwritten with the current tag of matching session.
	 */
	__u32 pxp_tag;

	/**
	 * @session_is_alive: Returns whether the session is alive in HW, based
	 * on the value in the KCR_SIP register.
	 */
	__u32 session_is_alive;
} __attribute__((packed));

/*
 * struct drm_xe_prelim_pxp_io_message - Params to send/receive message to/from TEE.
 */
struct drm_xe_prelim_pxp_io_message {
	/** @msg_in: pointer to memory containing input message */
	__u64 msg_in;
	/** @msg_in_size: input message size */
	__u32 msg_in_size;
	/** @msg_out: pointer to memory to store the output message */
	__u64 msg_out;
	/* @msg_out_buf_size: size of the memory available for msg_out */
	__u32 msg_out_buf_size;
	/* @msg_out_ret_size: actual size of the message returned from the TEE */
	__u32 msg_out_ret_size;
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
#define DRM_XE_PRELIM_PXP_ACTION_SESSION_OP 1
#define DRM_XE_PRELIM_PXP_ACTION_QUERY_PXP_TAG 2
#define DRM_XE_PRELIM_PXP_ACTION_TEE_IO_MESSAGE 3

	/** @status: returned outcome of the operation */
	__u32 status;
#define DRM_XE_PRELIM_PXP_OP_STATUS_SUCCESS 0
#define DRM_XE_PRELIM_PXP_OP_STATUS_RETRY_REQUIRED 1
#define DRM_XE_PRELIM_PXP_OP_STATUS_SESSION_NOT_AVAILABLE 2
#define DRM_XE_PRELIM_PXP_OP_STATUS_POWER_OFF 3

	/*
	 * in/out: action-specific data. Must fill the structure matching the
	 * selected action.
	 */
	union {
		/** @query_handle: parameters for the HOST_SESSION_HANDLE_REQ action */
		struct drm_xe_prelim_pxp_query_host_session_handle query_handle;
		/** @session_op: parameters for the SESSION_OP action */
		struct drm_xe_prelim_pxp_session_op session_op;
		/** @query_tag: parameters for the QUERY_PXP_TAG action */
		struct drm_xe_prelim_pxp_query_tag query_tag;
		/** @io_message: parameters for the TEE_IO_MESSAGE action */
		struct drm_xe_prelim_pxp_io_message io_message;
	};
} __attribute__((packed));

#endif /* __XE_DRM_PRELIM_H__ */

