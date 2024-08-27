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

void xe_pxp_close(struct xe_pxp *pxp, struct drm_file *drmfile)
{
	struct xe_pxp_client *client;

	if (!xe_pxp_is_enabled(pxp))
		return;

	mutex_lock(&pxp->multi_session.mutex);

	client = find_client(pxp, drmfile);

	if (client) {
		xe_pxp_destroy_client_resources(pxp, &client->res);
		list_del(&client->link);
		kfree(client);
	}

	mutex_unlock(&pxp->multi_session.mutex);
}

static void
pxp_query_host_session_handle(struct drm_xe_prelim_pxp_query_host_session_handle *query_handle,
			      struct xe_pxp_client *client)
{
	query_handle->host_session_handle = client->res.host_session_handle;
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

	if (XE_IOCTL_DBG(xe, action > DRM_XE_PRELIM_PXP_ACTION_HOST_SESSION_HANDLE_REQ))
		return -EINVAL;

	mutex_lock(&pxp->mutex);
	mutex_lock(&pxp->multi_session.mutex);

	client = xe_pxp_alloc_client_resources(pxp, drmfile);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		goto out_unlock;
	}

	switch (pxp_ops->action) {
	case DRM_XE_PRELIM_PXP_ACTION_HOST_SESSION_HANDLE_REQ:
		pxp_query_host_session_handle(&pxp_ops->query_handle, client);
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
	mutex_unlock(&pxp->mutex);

	return ret;
}

void xe_pxp_multi_session_init(struct xe_pxp *pxp)
{
	INIT_LIST_HEAD(&pxp->multi_session.client_list);
	mutex_init(&pxp->multi_session.mutex);
}
