/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef __MTK_APUEXT_EXPORT_H__
#define __MTK_APUEXT_EXPORT_H__

#include "mdw.h"

int mdw_ext_init(struct mdw_device *mdw_dev);
void mdw_ext_deinit(void);

void mdw_ext_lock(void);
void mdw_ext_unlock(void);

void mdw_ext_cmd_get_id(struct mdw_cmd *c);
void mdw_ext_cmd_put_id(struct mdw_cmd *c);

#endif
