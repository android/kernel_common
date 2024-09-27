/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_APUMMU_COMMON_H__
#define __APUSYS_APUMMU_COMMON_H__

#include <linux/printk.h>
#include <linux/seq_file.h>

extern u32 g_ammu_klog;

enum {
	AMMU_ERR = 0x1,
	AMMU_WRN = 0x2,
	AMMU_INFO = 0x4,
	AMMU_DBG = 0x8,
	AMMU_VERBO = 0x10,
};

#define APUMMU_PREFIX "[apummu]"

static inline int ammu_log_level_check(int log_level)
{
	return g_ammu_klog & log_level;
}

#define AMMU_LOG_ERR(x, args...) \
	do { \
		if (ammu_log_level_check(AMMU_ERR)) \
			pr_info(APUMMU_PREFIX "[error] %s " x, __func__, ##args); \
	} while (0)

#define AMMU_LOG_WRN(x, args...) \
	do { \
		if (ammu_log_level_check(AMMU_WRN)) \
			pr_info(APUMMU_PREFIX "[warn] %s " x, __func__, ##args); \
	} while (0)

#define AMMU_LOG_INFO(x, args...) \
	do { \
		if (ammu_log_level_check(AMMU_INFO)) \
			pr_info(APUMMU_PREFIX "[info] %s " x, __func__, ##args); \
	} while (0)

#define AMMU_LOG_DBG(x, args...) \
	do { \
		if (ammu_log_level_check(AMMU_DBG)) \
			pr_info(APUMMU_PREFIX "[dbg] %s " x, __func__, ##args); \
	} while (0)

#define AMMU_LOG_VERBO(x, args...) \
	do { \
		if (ammu_log_level_check(AMMU_VERBO)) \
			pr_info(APUMMU_PREFIX "[verbo] %s " x, __func__, ##args); \
	} while (0)

/* For tracking sequence */
#define AMMU_FOOT_PRINT(x, args...) \
	pr_info(APUMMU_PREFIX "[FootPrint] %s " x, __func__, ##args)

#if IS_ENABLED(CONFIG_MTK_AEE_FEATURE)
#include <aee.h>
#define _ammu_exception(key, reason) \
	do { \
		char info[150];\
		if (snprintf(info, 150, "apummu:" reason) > 0) { \
			aee_kernel_exception(info, \
				"\nCRDISPATCH_KEY:%s\n", key); \
		} else { \
			AMMU_LOG_ERR("apu_ammu: %s snprintf fail(%d)\n", __func__, __LINE__); \
		} \
	} while (0)

#define apusys_ammu_exception(reason) _ammu_exception("APUSYS_APUMMU", reason)
#define apusys_hse_exception(reason)  _ammu_exception("APUSYS_HSE",    reason)
#define apusys_cbfc_exception(reason) _ammu_exception("APUSYS_CBFC",   reason)

#else
#define apusys_ammu_exception(reason)
#define apusys_hse_exception(reason)
#define apusys_cbfc_exception(reason)
#endif /* end of IS_ENABLED(CONFIG_MTK_AEE_FEATURE) */


#endif /* end of __APUSYS_APUMMU_COMMON_H__ */
