// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include "linux/soc/mediatek/mtk_apu_device.h"

#include "apummu_cmn.h"
#include "linux/soc/mediatek/apummu_export.h"
#include "apummu_drv.h"
#include "apummu_remote_cmd.h"
#include "apummu_import.h"
#include "apummu_mgt.h"

#include "linux/soc/mediatek/apu_mem_export.h"

extern struct apummu_dev_info *g_adv;

static struct apu_mem_export_ops apummu_export_ops = {
	.apu_mem_alloc    = apummu_alloc_mem,
	.apu_mem_free     = apummu_free_mem,
	.apu_mem_import   = apummu_import_mem,
	.apu_mem_unimport = apummu_unimport_mem,
	.apu_mem_map      = apummu_map_mem,
	.apu_mem_unmap    = apummu_unmap_mem,


	.apu_mem_map_iova      = apummu_iova2eva,
	.apu_mem_iova_decode   = apummu_eva2iova,
	.apu_mem_unmap_iova    = apummu_buffer_remove,
	.apu_mem_table_get     = apummu_table_get,
	.apu_mem_table_free    = apummu_table_free,
	.apu_mem_DRAM_FB_alloc = apummu_DRAM_FB_alloc
};

int apummu_alloc_mem(uint32_t type, uint32_t size, uint64_t *addr, uint32_t *sid)
{
	int ret = 0;
	uint64_t ret_addr = 0, ret_size = 0;

	if (g_adv == NULL) {
		AMMU_LOG_ERR("Invalid apummu_device\n");
		ret = -EINVAL;
		return ret;
	}

	mutex_lock(&g_adv->plat.slb_mtx);

	switch (type) {
	case APUMMU_MEM_TYPE_EXT:
		if (g_adv->plat.external_SLB_cnt) {
			AMMU_LOG_WRN("External SLB is already alloced(%u->%u)\n",
				g_adv->plat.external_SLB_cnt, g_adv->plat.external_SLB_cnt+1);
			/* Still return to prevent unexcept user behavior */
			ret_addr = g_adv->rsc.external_SLB.iova;
			ret_size = g_adv->rsc.external_SLB.size;
			g_adv->plat.external_SLB_cnt++;
			goto out;
		}

		/* Put alloc inside to prevent alloc twice */
		ret = apummu_alloc_slb(type, size, g_adv->plat.slb_wait_time,
				&ret_addr, &ret_size);
		if (ret)
			goto err;

		g_adv->rsc.external_SLB.iova = ret_addr;
		g_adv->rsc.external_SLB.size = (uint32_t) ret_size;
		AMMU_LOG_DBG("External SLB allocate(%u->%u)\n",
			g_adv->plat.external_SLB_cnt, g_adv->plat.external_SLB_cnt+1);
		g_adv->plat.external_SLB_cnt++;
		break;
	case APUMMU_MEM_TYPE_RSV_S:
		if (g_adv->plat.internal_SLB_cnt) {
			AMMU_LOG_WRN("Internal SLB is already alloced(%u->%u)\n",
				g_adv->plat.internal_SLB_cnt, g_adv->plat.internal_SLB_cnt+1);
			/* Still return to prevent unexcept user behavior */
			ret_addr = g_adv->rsc.internal_SLB.iova;
			ret_size = g_adv->rsc.internal_SLB.size;
			g_adv->plat.internal_SLB_cnt++;
			goto out;
		}

		/* Put alloc inside to prevent alloc twice */
		ret = apummu_alloc_slb(type, size, g_adv->plat.slb_wait_time,
				&ret_addr, &ret_size);
		if (ret)
			goto err;

		g_adv->rsc.internal_SLB.iova = ret_addr;
		g_adv->rsc.internal_SLB.size = (uint32_t) ret_size;
		AMMU_LOG_DBG("Internal SLB allocate(%u->%u)\n",
			g_adv->plat.internal_SLB_cnt, g_adv->plat.internal_SLB_cnt+1);
		g_adv->plat.internal_SLB_cnt++;
		break;
	case APUMMU_MEM_TYPE_RSV_T:
		ret_addr = g_adv->remote.TCM_base_addr;
		ret_size = g_adv->remote.general_SRAM_size;
		break;
	case APUMMU_MEM_TYPE_VLM:
		ret_addr = g_adv->remote.vlm_addr;
		ret_size = g_adv->remote.vlm_size;
		break;
	default:
		AMMU_LOG_ERR("Invalid type %u\n", type);
		ret = -EINVAL;
		goto err;
	}

	AMMU_LOG_DBG("[Alloc][Done] Mem type(%u), addr(0x%llx), size(0x%llx)\n",
				type, ret_addr, ret_size);

out:
	mutex_unlock(&g_adv->plat.slb_mtx);
	*addr = ret_addr;
	*sid = type;

	return ret;

err:
	mutex_unlock(&g_adv->plat.slb_mtx);
	AMMU_LOG_ERR("[Alloc][Fail] Mem type(%u), addr(0x%llx), size(0x%llx)\n",
				type, ret_addr, ret_size);
	apusys_ammu_exception("alloc SLB fail\n");
	return ret;
}

int apummu_free_mem(uint32_t sid)
{
	int ret = 0;
	uint32_t type = sid;

	if (g_adv == NULL) {
		AMMU_LOG_ERR("Invalid apummu_device\n");
		ret = -EINVAL;
		return ret;
	}

	mutex_lock(&g_adv->plat.slb_mtx);

	switch (type) {
	case APUMMU_MEM_TYPE_EXT:
	case APUMMU_MEM_TYPE_RSV_S:
		if (type == APUMMU_MEM_TYPE_EXT) {
			if (!g_adv->plat.external_SLB_cnt) {
				AMMU_LOG_WRN("External SLB is not alloced\n");
				goto out;
			}

			g_adv->plat.external_SLB_cnt--;
			if (g_adv->plat.external_SLB_cnt) {
				AMMU_LOG_DBG("External SLB cnt(%u->%u), dont release\n",
					g_adv->plat.external_SLB_cnt+1, g_adv->plat.external_SLB_cnt);
				goto out;
			}

			AMMU_LOG_DBG("release External SLB cnt(%u)\n", g_adv->plat.external_SLB_cnt);
			//g_adv->plat.is_external_SLB_alloc = false;
			g_adv->rsc.external_SLB.iova = 0;
			g_adv->rsc.external_SLB.size = 0;
		} else {
			if (!g_adv->plat.internal_SLB_cnt) {
				AMMU_LOG_WRN("Internal SLB is not alloced\n");
				goto out;
			}

			g_adv->plat.internal_SLB_cnt--;
			if (g_adv->plat.internal_SLB_cnt) {
				AMMU_LOG_DBG("Internal SLB cnt(%u->%u), dont release\n",
					g_adv->plat.internal_SLB_cnt+1, g_adv->plat.internal_SLB_cnt);
				goto out;
			}

			AMMU_LOG_DBG("release Internal SLB cnt(%u)\n", g_adv->plat.internal_SLB_cnt);
			//g_adv->plat.is_internal_SLB_alloc = false;
			g_adv->rsc.internal_SLB.iova = 0;
			g_adv->rsc.internal_SLB.size = 0;
		}

		ret = apummu_free_slb(type);
		if (ret)
			goto err;

		ammu_session_table_check_SLB(type);
		break;
	case APUMMU_MEM_TYPE_VLM:
	case APUMMU_MEM_TYPE_RSV_T:
		break;
	default:
		AMMU_LOG_ERR("Invalid type %u\n", type);
		ret = -EINVAL;
		goto err;
	}

	AMMU_LOG_DBG("[Free][Done] Mem type(%u)\n", type);

out:
	mutex_unlock(&g_adv->plat.slb_mtx);
	return ret;

err:
	mutex_unlock(&g_adv->plat.slb_mtx);
	AMMU_LOG_ERR("[Free][Fail] Mem type(%u)\n", type);
	apusys_ammu_exception("free SLB fail\n");
	return ret;
}

int apummu_import_mem(uint64_t session, uint32_t type)
{
	int ret = 0;

	if (g_adv == NULL) {
		AMMU_LOG_ERR("Invalid apummu_device\n");
		ret = -EINVAL;
		return ret;
	}

	switch (type) {
	case APUMMU_MEM_TYPE_EXT:
	case APUMMU_MEM_TYPE_RSV_S:
		ret = ammu_session_table_add_SLB(session, type);
		if (ret)
			goto err;

		break;
	case APUMMU_MEM_TYPE_VLM:
	case APUMMU_MEM_TYPE_RSV_T:
		break;
	default:
		AMMU_LOG_ERR("Invalid type %u\n", type);
		ret = -EINVAL;
		goto err;
	}

	AMMU_LOG_DBG("[Import][Done] Mem session(0x%llx), type(%u)\n", session, type);
	return ret;

err:
	AMMU_LOG_ERR("[Import][Fail] Mem session(0x%llx), type(%u)\n", session, type);
	return ret;
}

int apummu_unimport_mem(uint64_t session, uint32_t type)
{
	int ret = 0;

	if (g_adv == NULL) {
		AMMU_LOG_ERR("Invalid apummu_device\n");
		ret = -EINVAL;
		return ret;
	}

	switch (type) {
	case APUMMU_MEM_TYPE_EXT:
	case APUMMU_MEM_TYPE_RSV_S:
		ret = ammu_session_table_remove_SLB(session, type);
		if (ret)
			goto err;

		break;
	case APUMMU_MEM_TYPE_VLM:
	case APUMMU_MEM_TYPE_RSV_T:
		break;
	default:
		AMMU_LOG_ERR("Invalid type %u\n", type);
		ret = -EINVAL;
		goto err;
	}

	AMMU_LOG_DBG("[Unimport][Done] Mem session(0x%llx), type(%u)\n", session, type);
	return ret;

err:
	AMMU_LOG_ERR("[Unimport][Fail] Mem session(0x%llx), type(%u)\n", session, type);
	return ret;
}

int apummu_map_mem(uint64_t session, uint32_t sid, uint64_t *addr)
{
	int ret = 0;
	uint64_t ret_addr = 0;
	uint32_t type = sid;

	if (g_adv == NULL) {
		AMMU_LOG_ERR("Invalid apummu_device\n");
		ret = -EINVAL;
		return ret;
	}

	switch (type) {
	case APUMMU_MEM_TYPE_EXT:
	case APUMMU_MEM_TYPE_RSV_S:
		ret = ammu_session_table_add_SLB(session, type);
		if (ret)
			goto err;

		ret_addr = (type == APUMMU_MEM_TYPE_EXT)
				? g_adv->rsc.external_SLB.iova
				: g_adv->rsc.internal_SLB.iova;

		break;
	case APUMMU_MEM_TYPE_VLM:
	case APUMMU_MEM_TYPE_RSV_T:
		ret_addr = g_adv->remote.vlm_addr;
		break;
	default:
		AMMU_LOG_ERR("Invalid type %u\n", type);
		ret = -EINVAL;
		goto err;
	}

	*addr = ret_addr;

	AMMU_LOG_DBG("[Map][Done] Mem (0x%llx/0x%x/0x%llx)\n", session, type, ret_addr);
	return ret;

err:
	AMMU_LOG_ERR("[Map][Fail] Mem (0x%llx/0x%x/0x%llx)\n", session, type, ret_addr);
	return ret;
}

int apummu_unmap_mem(uint64_t session, uint32_t sid)
{
	int ret = 0;
	uint32_t type = sid;

	if (g_adv == NULL) {
		AMMU_LOG_ERR("Invalid apummu_device\n");
		ret = -EINVAL;
		return ret;
	}

	switch (type) {
	case APUMMU_MEM_TYPE_EXT:
	case APUMMU_MEM_TYPE_RSV_S:
		ret = ammu_session_table_remove_SLB(session, type);
		if (ret)
			goto err;

		break;
	case APUMMU_MEM_TYPE_VLM:
	case APUMMU_MEM_TYPE_RSV_T:
		break;
	default:
		AMMU_LOG_ERR("Invalid type %u\n", type);
		ret = -EINVAL;
		goto err;
	}

	AMMU_LOG_DBG("[Unmap][Done] Mem (0x%llx/0x%x)\n", session, sid);
	return ret;

err:
	AMMU_LOG_ERR("[Unmap][Fail] Mem (0x%llx/0x%x)\n", session, sid);
	return ret;
}

/**
 * @para:
 *  type		-> input buffer type, plz refer to enum AMMU_BUF_TYPE
 *  session		-> input session
 *  device_va	-> input device_va (gonna encode to eva)
 *  buf_size	-> size of the buffer
 *  eva			-> output eva
 * @description:
 *  encode input addr to eva according to buffer type
 *  for apummu, we also record translate info into session table
 */
int apummu_iova2eva(uint32_t type, uint64_t session, uint64_t device_va,
		uint64_t buf_size, uint64_t *eva)
{
	return addr_encode_and_write_stable(type, session, device_va, buf_size, eva);
}

/**
 * @para:
 *  eva		-> input eva (gonna decode to iova)
 *  iova	-> output iova
 * @description:
 *  decode input addr to iova according to buffer type
 */
int apummu_eva2iova(uint64_t eva, uint64_t *iova)
{
	return apummu_eva_decode(eva, iova, AMMU_DATA_BUF);
}

/**
 * @para:
 *  session		-> input session
 *  device_va	-> input device_va
 *  buf_size	-> size of the buffer
 * @description:
 *  remove the mapping setting of the given buffer
 */
int apummu_buffer_remove(uint64_t session, uint64_t device_va, uint32_t buf_size)
{
	return apummu_stable_buffer_remove(session, device_va, buf_size);
}

/**
 * @para:
 *  session		-> hint for searching target session table
 *  tbl_kva		-> returned addr for session rable
 *  size		-> returned session table size
 * @description:
 *  return the session table according to session
 */
int apummu_table_get(uint64_t session, void **tbl_kva, uint32_t *size)
{
	return get_session_table(session, tbl_kva, size);
}

/**
 * @para:
 *  session		-> hint for searching target session table
 * @description:
 *  delete the session table according to session
 */
int apummu_table_free(uint64_t session)
{
	return session_table_free(session);
}

int apummu_DRAM_FB_alloc(uint64_t session, uint32_t vlm_size, uint32_t subcmd_num)
{
	return ammu_DRAM_FB_alloc(session, vlm_size, subcmd_num);
}
EXPORT_SYMBOL_NS(apummu_DRAM_FB_alloc, MTK_APU_MEM);


int apummu_export_API_init(void)
{
	return apu_mem_op_init(&apummu_export_ops);
}
