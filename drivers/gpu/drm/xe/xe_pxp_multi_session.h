/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2024, Intel Corporation. All rights reserved.
 */

#ifndef __XE_PXP_MULTI_SESSION_H__
#define __XE_PXP_MULTI_SESSION_H__

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct xe_pxp;

int xe_pxp_ops_ioctl(struct drm_device *dev, void *data, struct drm_file *drmfile);
void xe_pxp_multi_session_init(struct xe_pxp *pxp);

void xe_pxp_close(struct xe_pxp *pxp, struct drm_file *drmfile);

#endif /* __XE_PXP_MULTI_SESSION_H__ */
