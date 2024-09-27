// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include "mdw_cmn.h"

int mdw_cmd_ioctl(struct mdw_fpriv *mpriv, void *kdata)
{
	return mpriv->mdev->plat_funcs->prepare_cmd(mpriv, kdata);
}

