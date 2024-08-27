// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2024 Intel Corporation.
 */

#include "xe_pxp_multi_session.h"

#include <uapi/drm/xe_drm.h>
#include <uapi/drm/xe_drm_prelim.h>

#include "abi/gsc_command_header_abi.h"
#include "xe_assert.h"
#include "xe_device.h"
#include "xe_macros.h"
#include "xe_pm.h"
#include "xe_pxp.h"
#include "xe_pxp_submit.h"
#include "xe_pxp_types.h"

/**
 * struct xe_pxp_client
 *
 * We dont need to allocate multiple execution resources (above struct)
 * for a single drm_client that is executing multiple PXP sessions.
 * So we use a link list of nodes indexed by the drmclient handle
 */
struct xe_pxp_client {
	/** @link: achor for linked list. */
	struct list_head link;
	/** @exec: session execution resource for a given client. */
	struct xe_pxp_gsc_client_resources res;
	/** @drmfile: drm_file handle for a given client. */
	struct drm_file *drmfile;
};

static struct xe_pxp_client *find_client(struct xe_pxp *pxp, struct drm_file *drmfile)
{
	struct xe_pxp_client *client;

	if (!drmfile)
		return NULL;

	lockdep_assert_held(&pxp->multi_session.mutex);

	list_for_each_entry(client, &pxp->multi_session.client_list, link)
		if (client->drmfile == drmfile)
			return client;

	return NULL;
}

static struct xe_pxp_client *xe_pxp_alloc_client_resources(struct xe_pxp *pxp,
							   struct drm_file *drmfile)
{
	struct xe_pxp_client *client;
	int ret;

	xe_assert(pxp->xe, drmfile);

	lockdep_assert_held(&pxp->multi_session.mutex);

	client = find_client(pxp, drmfile);
	if (client)
		return client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	ret = xe_pxp_allocate_client_resources(pxp, &client->res);
	if (ret) {
		kfree(client);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&client->link);
	client->drmfile = drmfile;
	list_add_tail(&client->link, &pxp->multi_session.client_list);

	return client;
}

static int pxp_terminate_session(struct xe_pxp *pxp,
				 struct xe_pxp_gsc_client_resources *gsc_res, u32 id)
{
	int ret;

	lockdep_assert_held(&pxp->multi_session.mutex);

	/* terminate the hw session */
	ret = xe_pxp_submit_session_termination(pxp, BIT(id));
	if (ret)
		goto out;

	ret = xe_pxp_wait_for_session_state(pxp, id, false);
	if (ret)
		goto out;

	/* now we can tell the GSC to clean up its own state */
	ret = xe_pxp_submit_session_invalidation(gsc_res, id);

out:
	if (ret)
		drm_err(&pxp->xe->drm, "failed to kill PXP session %u, ret=%d\n", id, ret);

	return ret;
}

static void pxp_close_client_sessions(struct xe_pxp *pxp, struct xe_pxp_client *client)
{
	int idx;
	int ret;

	for_each_set_bit(idx, pxp->multi_session.reserved_sessions, INTEL_PXP_MAX_HWDRM_SESSIONS) {
		if (pxp->multi_session.sessions[idx].owner == client->drmfile) {
			ret = pxp_terminate_session(pxp, &client->res, idx);
			if (ret)
				drm_err(&pxp->xe->drm,
					"failed to correctly close PXP session %u\n",
					idx);

			pxp->multi_session.sessions[idx].owner = NULL;
			clear_bit(idx, pxp->multi_session.reserved_sessions);
		}
	}
}

void xe_pxp_close(struct xe_pxp *pxp, struct drm_file *drmfile)
{
	struct xe_pxp_client *client;

	if (!xe_pxp_is_enabled(pxp))
		return;

	mutex_lock(&pxp->multi_session.mutex);

	client = find_client(pxp, drmfile);

	if (client) {
		pxp_close_client_sessions(pxp, client);

		xe_pxp_destroy_client_resources(pxp, &client->res);
		list_del(&client->link);
		kfree(client);
	}

	mutex_unlock(&pxp->multi_session.mutex);
}

static u32 __pxp_tag(struct xe_pxp *pxp, int idx, int mode, u8 instance)
{
	u32 pxp_tag = 0;

	switch (mode) {
	case DRM_XE_PRELIM_PXP_MODE_LM:
		break;
	case DRM_XE_PRELIM_PXP_MODE_HM:
		pxp_tag |= DRM_XE_PRELIM_PXP_TAG_SESSION_HM;
		break;
	case DRM_XE_PRELIM_PXP_MODE_SM:
		pxp_tag |= DRM_XE_PRELIM_PXP_TAG_SESSION_HM;
		pxp_tag |= DRM_XE_PRELIM_PXP_TAG_SESSION_SM;
		break;
	default:
		drm_err(&pxp->xe->drm, "unexpected PXP protection mode %d\n", mode);
	}

	pxp_tag |= DRM_XE_PRELIM_PXP_TAG_SESSION_ENABLED;
	pxp_tag |= FIELD_PREP(DRM_XE_PRELIM_PXP_TAG_INSTANCE_ID_MASK, instance);
	pxp_tag |= FIELD_PREP(DRM_XE_PRELIM_PXP_TAG_SESSION_ID_MASK, idx);

	return pxp_tag;
}

static u32 pxp_tag_fill(struct xe_pxp *pxp, int idx, int mode)
{
	u8 instance = ++pxp->multi_session.sessions[idx].instance;

	if (!instance)
		instance = ++pxp->multi_session.sessions[idx].instance;

	return __pxp_tag(pxp, idx, mode, instance);
}

static int pxp_reserve_session(struct xe_pxp *pxp, struct xe_pxp_client *client,
			       u32 type, u32 mode, u32 *pxp_tag)
{
	int ret;
	int idx = 0;

	lockdep_assert_held(&pxp->multi_session.mutex);

	if (XE_IOCTL_DBG(pxp->xe, type != DRM_XE_PXP_TYPE_HWDRM))
		return -EINVAL;

	if (XE_IOCTL_DBG(pxp->xe,
			 mode < DRM_XE_PRELIM_PXP_MODE_LM || mode > DRM_XE_PRELIM_PXP_MODE_SM))
		return -EINVAL;

	idx = find_first_zero_bit(pxp->multi_session.reserved_sessions,
				  INTEL_PXP_MAX_HWDRM_SESSIONS);
	if (idx >= INTEL_PXP_MAX_HWDRM_SESSIONS)
		return DRM_XE_PRELIM_PXP_OP_STATUS_SESSION_NOT_AVAILABLE;

	ret = xe_pxp_wait_for_session_state(pxp, idx, false);
	if (ret) {
		/* force termination of old reservation */
		ret = pxp_terminate_session(pxp, &client->res, idx);
		if (ret) {
			/* mark the buggy session as reserved so we stop using it */
			set_bit(idx, pxp->multi_session.reserved_sessions);
			return DRM_XE_PRELIM_PXP_OP_STATUS_RETRY_REQUIRED;
		}
	}

	set_bit(idx, pxp->multi_session.reserved_sessions);
	pxp->multi_session.sessions[idx].owner = client->drmfile;
	*pxp_tag = pxp_tag_fill(pxp, idx, mode);

	return ret;
}

static int pxp_release_session(struct xe_pxp *pxp,
			       struct xe_pxp_client *client,
			       u32 session_id)
{
	int ret;

	lockdep_assert_held(&pxp->multi_session.mutex);

	if (session_id >= INTEL_PXP_MAX_HWDRM_SESSIONS)
		return -EINVAL;

	/* already gone */
	if (!test_bit(session_id, pxp->multi_session.reserved_sessions))
		return 0;

	if (pxp->multi_session.sessions[session_id].owner != client->drmfile)
		return -EPERM;

	ret = pxp_terminate_session(pxp, &client->res, session_id);
	if (ret)
		return ret;

	pxp->multi_session.sessions[session_id].owner = NULL;
	clear_bit(session_id, pxp->multi_session.reserved_sessions);

	return 0;
}

static int pxp_session_op(struct xe_pxp *pxp,
			  struct drm_xe_prelim_pxp_session_op *session_op,
			  struct xe_pxp_client *client)
{
	u32 session_id;
	int ret = 0;

	switch (session_op->action) {
	case DRM_XE_PRELIM_PXP_SESSION_RESERVE:
		/* session id must be empty when reserving */
		if (XE_IOCTL_DBG(pxp->xe, session_op->pxp_tag))
			return -EINVAL;

		ret = pxp_reserve_session(pxp, client,
					  session_op->session_type,
					  session_op->session_mode,
					  &session_op->pxp_tag);
		break;
	case DRM_XE_PRELIM_PXP_SESSION_RELEASE:
		session_id = session_op->pxp_tag & DRM_XE_PRELIM_PXP_TAG_SESSION_ID_MASK;

		ret = pxp_release_session(pxp, client, session_id);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static void
pxp_query_host_session_handle(struct drm_xe_prelim_pxp_query_host_session_handle *query_handle,
			      struct xe_pxp_client *client)
{
	query_handle->host_session_handle = client->res.host_session_handle;
}

static bool pxp_op_needs_rpm(u32 op)
{
	return op != DRM_XE_PRELIM_PXP_ACTION_HOST_SESSION_HANDLE_REQ;
}

static bool pxp_op_needs_arb(u32 op)
{
	return op != DRM_XE_PRELIM_PXP_ACTION_HOST_SESSION_HANDLE_REQ;
}

int xe_pxp_ops_ioctl(struct drm_device *dev, void *data, struct drm_file *drmfile)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_pxp *pxp = xe->pxp;
	struct drm_xe_prelim_pxp_ops *pxp_ops = data;
	u32 action = pxp_ops->action;
	struct xe_pxp_client *client;
	int ret = 0;

	if (!xe_pxp_is_enabled(pxp))
		return -ENODEV;

	if (XE_IOCTL_DBG(xe, pxp_ops->extensions))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, action > DRM_XE_PRELIM_PXP_ACTION_SESSION_OP))
		return -EINVAL;

	if (pxp_op_needs_rpm(action) && !xe_pm_runtime_get_if_in_use(xe)) {
		drm_dbg(&xe->drm, "pxp ioctl blocked due to hw suspend\n");
		pxp_ops->status = DRM_XE_PRELIM_PXP_OP_STATUS_POWER_OFF;
		return 0;
	}

	if (pxp_op_needs_arb(action)) {
		xe_assert(xe, pxp_op_needs_rpm(action));

pxp_start:
		/* This will wait for any pending termination to complete */
		ret = xe_pxp_start(pxp, DRM_XE_PXP_TYPE_HWDRM, NULL);
		if (ret)
			goto out_pm;
	}

	mutex_lock(&pxp->multi_session.mutex);

	/*
	 * check if a new termination was issued between the above check and
	 * grabbing the mutex
	 */
	if (pxp_op_needs_arb(action) && !completion_done(&pxp->termination)) {
		mutex_unlock(&pxp->multi_session.mutex);
		goto pxp_start;
	}

	client = xe_pxp_alloc_client_resources(pxp, drmfile);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		goto out_unlock;
	}

	switch (pxp_ops->action) {
	case DRM_XE_PRELIM_PXP_ACTION_HOST_SESSION_HANDLE_REQ:
		pxp_query_host_session_handle(&pxp_ops->query_handle, client);
		break;
	case DRM_XE_PRELIM_PXP_ACTION_SESSION_OP:
		ret = pxp_session_op(pxp, &pxp_ops->session_op, client);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret >= 0) {
		pxp_ops->status = ret;
		ret = 0;
	}

out_unlock:
	mutex_unlock(&pxp->multi_session.mutex);
out_pm:
	if (pxp_op_needs_rpm(action))
		xe_pm_runtime_put(xe);

	return ret;
}

void xe_pxp_multi_session_init(struct xe_pxp *pxp)
{
	INIT_LIST_HEAD(&pxp->multi_session.client_list);
	mutex_init(&pxp->multi_session.mutex);

	/* The default session is perma-reserved by the kernel */
	set_bit(DRM_XE_PXP_HWDRM_DEFAULT_SESSION, pxp->multi_session.reserved_sessions);
}

void xe_pxp_invalidate_sessions(struct xe_pxp *pxp, u32 mask)
{
	int i;

	for (i = 0; i < INTEL_PXP_MAX_HWDRM_SESSIONS; i++) {
		struct xe_pxp_client *client;
		struct xe_pxp_gsc_client_resources *gsc_res;

		if (!(mask & BIT(i)))
			continue;

		lockdep_assert_held(&pxp->multi_session.mutex);

		client = find_client(pxp, pxp->multi_session.sessions[i].owner);

		if (client)
			gsc_res = &client->res;
		else
			gsc_res = &pxp->gsc_res;

		xe_pxp_submit_session_invalidation(gsc_res, i);
		/* TODO check ret? */
	}
}
