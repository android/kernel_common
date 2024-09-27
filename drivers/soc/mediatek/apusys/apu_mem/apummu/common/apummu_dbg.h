/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_APUMMU_DEBUG_H__
#define __APUSYS_APUMMU_DEBUG_H__

#include <linux/debugfs.h>

void apummu_dbg_init(struct apummu_dev_info *rdv, struct dentry *apu_dbg_root);
void apummu_dbg_destroy(struct apummu_dev_info *rdv);

#endif
