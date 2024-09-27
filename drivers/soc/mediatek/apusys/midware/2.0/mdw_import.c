// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include "mdw_cmn.h"
#include "mdw_import.h"
#include "mdw_trace.h"
#include "linux/soc/mediatek/apummu_export.h"
#include "linux/soc/mediatek/apummu_mem_def.h"
#include "linux/soc/mediatek/apu_mem_export.h"
#include "linux/soc/mediatek/apu_mem_def.h"

bool mdw_pwr_check(void)
{
	// return apusys_power_check();
	return true;
}

static int mdw_mem_type_convert(uint32_t type, uint32_t *out)
{
	switch (type) {
	case MDW_MEM_TYPE_VLM:
		*out = APUMMU_MEM_TYPE_VLM;
		break;
	case MDW_MEM_TYPE_LOCAL:
		*out = APUMMU_MEM_TYPE_RSV_T;
		break;
	case MDW_MEM_TYPE_SYSTEM_ISP:
		*out = APUMMU_MEM_TYPE_EXT;
		break;
	case MDW_MEM_TYPE_SYSTEM_APU:
		*out = APUMMU_MEM_TYPE_RSV_S;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

int mdw_apu_mem_alloc(uint32_t type, uint32_t size,
	uint64_t *addr, uint32_t *sid)
{
	uint32_t mapped_type = 0;
	int ret = 0;

	if (mdw_mem_type_convert(type, &mapped_type))
		return -EINVAL;

	ret = apu_mem_alloc(mapped_type, size, addr, sid);
	mdw_flw_debug("type(%u->%u)size(%u)addr(0x%llx)sid(%u)\n",
		type, mapped_type, size, *addr, *sid);

	return ret;
}

int mdw_apu_mem_free(uint32_t sid)
{
	mdw_flw_debug("sid(%u)\n", sid);
	return apu_mem_free(sid);
}

int mdw_apu_mem_import(uint64_t session, uint32_t sid)
{
	mdw_flw_debug("s(0x%llx)sid(%u)\n", (uint64_t)session, sid);
	return apu_mem_import(session, sid);
}

int mdw_apu_mem_unimport(uint64_t session, uint32_t sid)
{
	mdw_flw_debug("s(0x%llx)sid(%u)\n", (uint64_t)session, sid);
	return apu_mem_unimport(session, sid);
}

int mdw_apu_mem_map(uint64_t session, uint32_t sid, uint64_t *vaddr)
{
	int ret = 0;

	ret = apu_mem_map(session, sid, vaddr);
	mdw_flw_debug("s(0x%llx)sid(%u)vaddr(0x%llx)\n",
		session, sid, *vaddr);

	return ret;
}

int mdw_apu_mem_unmap(uint64_t session, uint32_t sid)
{
	mdw_flw_debug("s(0x%llx)sid(%u)\n", (uint64_t)session, sid);
	return apu_mem_unmap(session, sid);
}

int mdw_ammu_eva2iova(uint64_t eva, uint64_t *iova)
{
	return apu_mem_iova_decode(eva, iova);
}

// int mdw_qos_cmd_start(uint64_t cmd_id, uint64_t sc_id,
//		int type, int core, uint32_t boost)
// {
//	return apu_cmd_qos_start(cmd_id, sc_id, type, core, boost);
// }

// int mdw_qos_cmd_end(uint64_t cmd_id, uint64_t sc_id,
//		int type, int core)
// {
//	return apu_cmd_qos_end(cmd_id, sc_id, type, core);
// }
