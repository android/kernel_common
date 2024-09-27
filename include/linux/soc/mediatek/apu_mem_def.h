/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_APU_MEM_DEF_H__
#define __APUSYS_APU_MEM_DEF_H__

struct apu_mem_export_ops {
	/* Common API */
	int (*apu_mem_alloc)    (uint32_t type, uint32_t size, uint64_t *addr,
							uint32_t *sid);
	int (*apu_mem_free)     (uint32_t sid);
	int (*apu_mem_import)   (uint64_t session, uint32_t sid);
	int (*apu_mem_unimport) (uint64_t session, uint32_t sid);
	int (*apu_mem_map)      (uint64_t session, uint32_t sid, uint64_t *addr);
	int (*apu_mem_unmap)    (uint64_t session, uint32_t sid);

	/* Reviser only API */
	int (*apu_mem_rvs_get_vlm)         (uint32_t request_size, bool force,
										unsigned long *ctx, uint32_t *tcm_size);
	int (*apu_mem_rvs_free_vlm)        (uint32_t ctx);
	int (*apu_mem_rvs_set_context)     (int type, int index, uint8_t ctx);
	int (*apu_mem_rvs_get_resource_vlm)(uint32_t *addr, uint32_t *size);
	int (*apu_mem_rvs_get_pool_size)   (uint32_t type, uint32_t *size);

	/* APUMMU only API */
	int (*apu_mem_map_iova)     (uint32_t type, uint64_t session, uint64_t device_va,
								uint64_t buf_size, uint64_t *eva);
	int (*apu_mem_iova_decode)  (uint64_t eva, uint64_t *iova);
	int (*apu_mem_unmap_iova)   (uint64_t session, uint64_t device_va, uint32_t buf_size);
	int (*apu_mem_table_get)    (uint64_t session, void **tbl_kva, uint32_t *size);
	int (*apu_mem_table_free)   (uint64_t session);
	int (*apu_mem_DRAM_FB_alloc)(uint64_t session, uint32_t vlm_size, uint32_t subcmd_num);
};

enum APU_MEM_TYPE {
	/* memory type */
	APU_MEM_TYPE_NONE = 0x0,
	APU_MEM_TYPE_DRAM,
	APU_MEM_TYPE_TCM,
	APU_MEM_TYPE_SLBS,
	APU_MEM_TYPE_VLM,
	APU_MEM_TYPE_RSV_T,
	APU_MEM_TYPE_RSV_S,
	APU_MEM_TYPE_EXT,
	APU_MEM_TYPE_GENERAL_S,
	APU_MEM_TYPE_MAX
};

#endif /* end of __APUSYS_APU_MEM_DEF_H__ */
