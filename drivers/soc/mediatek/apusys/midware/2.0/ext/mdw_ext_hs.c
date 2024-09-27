// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include "mdw_ext.h"
#include "mdw_ext_cmn.h"
#include "mdw_ext_ioctl.h"

#define MDW_EXT_SUPPORT_VER (4)

static int mdw_ext_hs_version(union mdw_ext_hs_args *args)
{
		memset(args, 0, sizeof(*args));
		args->out.out0 = MDW_EXT_SUPPORT_VER;
		mdwext_drv_debug("ext hs version(%llu)\n", args->out.out0);

	return 0;
}

int mdw_ext_hs_ioctl(void *data)
{
	union mdw_ext_hs_args *args = (union mdw_ext_hs_args *)data;
	int ret = -EINVAL;

	mdwext_drv_debug("ext hs op(%llu)\n", args->in.op);
	switch (args->in.op) {
	case MDW_EXT_HS_TYPE_VERSION:
		ret = mdw_ext_hs_version(args);
		break;
	default:
		mdwext_drv_err("no support hs(%llu)\n", args->in.op);
		break;
	}
	mdwext_drv_debug("ext hs op(%llu) done\n", args->in.op);

	return ret;
}
