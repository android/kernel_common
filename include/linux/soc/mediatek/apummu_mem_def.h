/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __APUSYS_APUMMU_MEM_DEF_H__
#define __APUSYS_APUMMU_MEM_DEF_H__

#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>

enum APUMMU_MEM_TYPE {
	/* memory type */
	APUMMU_MEM_TYPE_NONE = 0x0,
	APUMMU_MEM_TYPE_DRAM,
	APUMMU_MEM_TYPE_TCM,
	APUMMU_MEM_TYPE_SLBS,
	APUMMU_MEM_TYPE_VLM,
	APUMMU_MEM_TYPE_RSV_T,
	APUMMU_MEM_TYPE_RSV_S,
	APUMMU_MEM_TYPE_EXT,
	APUMMU_MEM_TYPE_GENERAL_S,
	APUMMU_MEM_TYPE_MAX
};

#define USING_SELF_DMA               (0)
#if USING_SELF_DMA
struct ammu_mem_map {
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct kref map_ref;
	struct apummu_mem *m;
	void (*get)(struct ammu_mem_map *map);
	void (*put)(struct ammu_mem_map *map);
};

struct apummu_mem {
	uint64_t kva;
	uint64_t iova;
	uint32_t size;
	uint32_t dva_size;

	void *vaddr;
	void *dbuf;
	void *priv;
	struct mutex mtx;
	struct ammu_mem_map *map;
};
#else // using mm_heap
struct apummu_mem {
	uint64_t kva;
	uint64_t iova;
	uint32_t size;

	struct dma_heap *heap;
	void *priv;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct list_head list;
};
#endif
#endif
