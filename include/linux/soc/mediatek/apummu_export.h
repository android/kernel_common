/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_APUMMU_EXPORT_H__
#define __APUSYS_APUMMU_EXPORT_H__
#include <linux/types.h>

int apummu_alloc_mem(uint32_t type, uint32_t size, uint64_t *addr, uint32_t *sid);
int apummu_free_mem(uint32_t sid);
int apummu_import_mem(uint64_t session, uint32_t sid);
int apummu_unimport_mem(uint64_t session, uint32_t sid);
// remap? map PA -> VA
int apummu_map_mem(uint64_t session, uint32_t sid, uint64_t *addr);
int apummu_unmap_mem(uint64_t session, uint32_t sid);


/* add for APUMMU */

/**
 * @para:
 *  type		-> input buffer type, plz refer to enum AMMU_BUF_TYPE
 *  session		-> input session
 *  device_va	-> input device_va (gonna encode to eva)
 *  buf_size	-> input buffer size(for EVA encode)
 *  eva			-> output eva
 * @description:
 *  encode input addr to eva according to buffer type
 *  for apummu, we also record translate info into session table
 */
int apummu_iova2eva(uint32_t type, uint64_t session, uint64_t device_va,
			uint64_t buf_size, uint64_t *eva);

int apummu_eva2iova(uint64_t eva, uint64_t *iova);

/**
 * @para:
 *  session	-> input session
 *  device_va	-> input device_va
 *  buf_size	-> size of the buffer
 * @description:
 *  remove the mapping setting of the given buffer
 */
int apummu_buffer_remove(uint64_t session, uint64_t device_va, uint32_t buf_size);

/**
 * @para:
 *  session		-> hint for searching target session table
 *  tbl_kva		-> returned addr for session rable
 *  size		-> returned session table size
 * @description:
 *  return the session table according to session
 */
int apummu_table_get(uint64_t session, void **tbl_kva, uint32_t *size);

/**
 * @para:
 *  session		-> hint for searching target session table
 * @description:
 *  delete the session table according to session
 */
int apummu_table_free(uint64_t session);

/**
 * @para:
 *  session    -> session id
 *  vlm_size   -> sum of the vlm size in the APUCMD
 *  subcmd_num -> sum of the subcmd number in the APUCMD
 * @description:
 *  VLM DRAM FB allocated according to vlm_size and subcmd_num
 */
int apummu_DRAM_FB_alloc(uint64_t session, uint32_t vlm_size, uint32_t subcmd_num);

int apummu_export_API_init(void);
#endif
