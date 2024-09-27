/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef __MTK_APU_MDW_IMPORT_H__
#define __MTK_APU_MDW_IMPORT_H__

/* import function */
bool mdw_pwr_check(void);

int mdw_apu_mem_alloc(uint32_t type, uint32_t size,
	uint64_t *addr, uint32_t *sid);
int mdw_apu_mem_free(uint32_t sid);
int mdw_apu_mem_import(uint64_t session, uint32_t sid);
int mdw_apu_mem_unimport(uint64_t session, uint32_t sid);
int mdw_apu_mem_map(uint64_t session, uint32_t sid, uint64_t *vaddr);
int mdw_apu_mem_unmap(uint64_t session, uint32_t sid);

int mdw_ammu_eva2iova(uint64_t eva, uint64_t *iova);

int mdw_qos_cmd_start(uint64_t cmd_id, uint64_t sc_id,
		int type, int core, uint32_t boost);
int mdw_qos_cmd_end(uint64_t cmd_id, uint64_t sc_id,
		int type, int core);

#endif
