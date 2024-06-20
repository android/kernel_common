/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2023 MediaTek Inc.
 */

#define FDVFS_MAGICNUM	0xABCD0001
#define FDVFS_FREQU	26000
#define CPU_OFS	0x4

enum FDVFS_REG_TYPE {
	FDVFS_REG,
	FDVFS_CCI_REG,
	FDVFS_SUPPORT,
};

enum SupportState {
	UNSUPPORTED = -1,
	UNKNOWN = 0,
	SUPPORTED = 1,
};

int check_fdvfs_support(struct device *dev);
int cpufreq_fdvfs_switch(unsigned int target_f, struct cpufreq_policy *policy);
bool cpufreq_fdvfs_cci_switch(unsigned int target_f);
