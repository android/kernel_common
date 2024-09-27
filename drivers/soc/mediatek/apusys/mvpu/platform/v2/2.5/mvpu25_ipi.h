/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */
#ifndef __MVPU25_IPI_H__
#define __MVPU25_IPI_H__

#include <linux/types.h>

#if IS_ENABLED(CONFIG_MTK_AEE_FEATURE)
#include <mt-plat/aee.h>
#define mvpu_aee_exception(key, format, args...) \
	do { \
		pr_info(format, ##args); \
		aee_kernel_exception("MVPU", \
			"\nCRDISPATCH_KEY:" key "\n" format, ##args); \
	} while (0)
#else
#define mvpu_aee_exception(key, format, args...)
#endif

int mvpu25_ipi_init(void);
void mvpu25_ipi_deinit(void);
int mvpu25_ipi_send(uint32_t type, uint32_t dir, uint64_t *val);
int mvpu25a_ipi_init(void);
void mvpu25a_ipi_deinit(void);

#endif /* __MVPU25_IPI_H__ */

