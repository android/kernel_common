/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */
#ifndef __MVPU_CONFIG_H__
#define __MVPU_CONFIG_H__

#include "linux/remoteproc/mtk_apu.h"

int mvpu_config_init(struct mtk_apu *apu);
int mvpu_config_remove(struct mtk_apu *apu);

#endif
