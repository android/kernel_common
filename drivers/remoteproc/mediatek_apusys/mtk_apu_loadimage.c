// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/firmware.h>
#include <linux/of_address.h>

#include <linux/firmware/mediatek/mtk-apu.h>
#include <linux/remoteproc/mtk_apu.h>
#include <linux/remoteproc/mtk_apu_loadimage.h>

#include "mtk_apu_rproc.h"

#define MTK_APU_FW_HEADER_OFFSET	(0x200)

static int mtk_apusys_rv_load_fw(struct apusys_secure_info_t *sec_info,
				 void *sec_mem_addr_va, struct mtk_apu *apu,
				 const struct firmware *fw)
{
	unsigned int apusys_pmsize, apusys_xsize;
	const void *apusys_pmimg, *apusys_ximg;
	void *tmp_addr;
	const void *img;
	const struct ptimg_hdr_t *hdr;
	unsigned int *img_size;

	/* initialize apusys sec_info */
	tmp_addr = sec_info->up_code_buf_ofs + sec_info->up_code_buf_sz +
		   sec_mem_addr_va + roundup(sizeof(*sec_info), APUSYS_FW_ALIGN);

	/* separate ptimg */
	apusys_pmimg = apusys_ximg = NULL;
	apusys_pmsize = apusys_xsize = 0;

	hdr = (const struct ptimg_hdr_t *)(fw->data + MTK_APU_FW_HEADER_OFFSET);
	dev_dbg(apu->dev, "%s: hdr->magic is 0x%x\n", __func__, hdr->magic);
	img_size = (void *)fw->data + (0x4);
	dev_dbg(apu->dev, "%s: apu_partition_sz is 0x%x\n", __func__, *img_size);
	memcpy_toio(tmp_addr, ((void *)fw->data + (MTK_APU_FW_HEADER_OFFSET)), (*img_size));

	hdr = tmp_addr;

	while (hdr->magic == PT_MAGIC) {
		img = ((const void *) hdr) + hdr->hdr_size;
		dev_dbg(apu->dev, "Rhdr->hdr_size= 0x%x\n", hdr->hdr_size);
		dev_dbg(apu->dev, "img address is %p\n", img);

		switch (hdr->id) {
		case PT_ID_APUSYS_FW:
			dev_dbg(apu->dev, "PT_ID_APUSYS_FW\n");
			apusys_pmimg = img;
			apusys_pmsize = hdr->img_size;
			sec_info->up_fw_ofs = (u64)img - (u64)sec_mem_addr_va;
			sec_info->up_fw_sz = apusys_pmsize;
			dev_dbg(apu->dev, "up_fw_ofs = 0x%x, up_fw_sz = 0x%x\n",
				sec_info->up_fw_ofs, sec_info->up_fw_sz);
			break;
		case PT_ID_APUSYS_XFILE:
			dev_dbg(apu->dev, "PT_ID_APUSYS_XFILE\n");
			apusys_ximg = img;
			apusys_xsize = hdr->img_size;
			sec_info->up_xfile_ofs = (u64)hdr - (u64)sec_mem_addr_va;
			sec_info->up_xfile_sz = hdr->hdr_size + apusys_xsize;
			dev_dbg(apu->dev, "up_xfile_ofs = 0x%x, up_xfile_sz = 0x%x\n",
				sec_info->up_xfile_ofs, sec_info->up_xfile_sz);
			break;
		case PT_ID_MDLA_FW_BOOT:
			dev_dbg(apu->dev, "PT_ID_MDLA_FW_BOOT\n");
			sec_info->mdla_fw_boot_ofs = (u64)img - (u64)sec_mem_addr_va;
			sec_info->mdla_fw_boot_sz = hdr->img_size;
			dev_dbg(apu->dev, "mdla_fw_boot_ofs = 0x%x, mdla_fw_boot_sz = 0x%x\n",
				sec_info->mdla_fw_boot_ofs,
				sec_info->mdla_fw_boot_sz);
			break;
		case PT_ID_MDLA_FW_MAIN:
			dev_dbg(apu->dev, "PT_ID_MDLA_FW_MAIN\n");
			sec_info->mdla_fw_main_ofs = (u64)img - (u64)sec_mem_addr_va;
			sec_info->mdla_fw_main_sz = hdr->img_size;
			dev_dbg(apu->dev, "mdla_fw_main_ofs = 0x%x, mdla_fw_main_sz = 0x%x\n",
				sec_info->mdla_fw_main_ofs,
				sec_info->mdla_fw_main_sz);
			break;
		case PT_ID_MDLA_XFILE:
			dev_dbg(apu->dev, "PT_ID_MDLA_XFILE\n");
			sec_info->mdla_xfile_ofs = (u64)hdr - (u64)sec_mem_addr_va;
			sec_info->mdla_xfile_sz = hdr->hdr_size + hdr->img_size;
			dev_dbg(apu->dev, "mdla_xfile_ofs = 0x%x, mdla_xfile_sz = 0x%x\n",
				sec_info->mdla_xfile_ofs,
				sec_info->mdla_xfile_sz);
			break;
		case PT_ID_MVPU_FW:
			dev_dbg(apu->dev, "PT_ID_MVPU_FW\n");
			sec_info->mvpu_fw_ofs = (u64)img - (u64)sec_mem_addr_va;
			sec_info->mvpu_fw_sz = hdr->img_size;
			dev_dbg(apu->dev, "mvpu_fw_ofs = 0x%x, mvpu_fw_sz = 0x%x\n",
				sec_info->mvpu_fw_ofs, sec_info->mvpu_fw_sz);
			break;
		case PT_ID_MVPU_XFILE:
			dev_dbg(apu->dev, "PT_ID_MVPU_XFILE\n");
			sec_info->mvpu_xfile_ofs = (u64)hdr - (u64)sec_mem_addr_va;
			sec_info->mvpu_xfile_sz = hdr->hdr_size + hdr->img_size;
			dev_dbg(apu->dev, "mvpu_xfile_ofs = 0x%x, mvpu_xfile_sz = 0x%x\n",
				sec_info->mvpu_xfile_ofs,
				sec_info->mvpu_xfile_sz);
			break;
		case PT_ID_MVPU_SEC_FW:
			dev_dbg(apu->dev, "PT_ID_MVPU_SEC_FW\n");
			sec_info->mvpu_sec_fw_ofs = (u64)img - (u64)sec_mem_addr_va;
			sec_info->mvpu_sec_fw_sz = hdr->img_size;
			dev_dbg(apu->dev, "mvpu_sec_fw_ofs = 0x%x, mvpu_sec_fw_sz = 0x%x\n",
				sec_info->mvpu_sec_fw_ofs,
				sec_info->mvpu_sec_fw_sz);
			break;
		case PT_ID_MVPU_SEC_XFILE:
			dev_dbg(apu->dev, "PT_ID_MVPU_SEC_XFILE\n");
			sec_info->mvpu_sec_xfile_ofs = (u64)hdr - (u64)sec_mem_addr_va;
			sec_info->mvpu_sec_xfile_sz = hdr->hdr_size + hdr->img_size;
			dev_dbg(apu->dev, "mvpu_sec_xfile_ofs = 0x%x, mvpu_sec_xfile_sz = 0x%x\n",
				sec_info->mvpu_sec_xfile_ofs,
				sec_info->mvpu_sec_xfile_sz);
			break;
		case PT_ID_CE_BIN:
			dev_dbg(apu->dev, "PT_ID_CE_BIN\n");
			sec_info->ce_bin_ofs = (uint64_t) img - (uint64_t) sec_mem_addr_va;
			sec_info->ce_bin_sz = hdr->hdr_size + hdr->img_size;
			dev_dbg(apu->dev, "ce_bin_ofs = 0x%x, ce_bin_sz = 0x%x\n",
				sec_info->ce_bin_ofs, sec_info->ce_bin_sz);
			break;
		case PT_ID_CE_BIN_DUMMY:
			/* dummy image do nothing */
			break;
		default:
			dev_warn(apu->dev, "Ignore unknown APUSYS image %d\n", hdr->id);
			break;
		}

		dev_dbg(apu->dev, "hdr->img_size = 0x%x, roundup(hdr->img_size, hdr->align) = 0x%x\n",
			hdr->img_size, roundup(hdr->img_size, hdr->align));
		img += roundup(hdr->img_size, hdr->align);
		hdr = (const struct ptimg_hdr_t *)img;
	}

	if (!apusys_pmimg || !apusys_ximg) {
		dev_err(apu->dev, "APUSYS partition missing - PM: %p, XM: %p (@%p)\n",
				apusys_pmimg, apusys_ximg, tmp_addr);
		return -ENOENT;
	}

	dev_dbg(apu->dev, "APUSYS part load finished: PM: %p(up_fw_ofs = 0x%x), XM: %p(up_xfile_ofs = 0x%x)\n",
		apusys_pmimg, sec_info->up_fw_ofs, apusys_ximg, sec_info->up_xfile_ofs);

	return 0;
}

static int mtk_apusys_rv_fill_sec_info(struct apusys_secure_info_t *sec_info,
				       const struct mtk_apu_plat_config *config,
				       uint64_t apusys_part_size, struct mtk_apu *apu)
{
	uint64_t coredump_buf_sz = 0, sec_mem_sz = 0;

	sec_info->up_code_buf_ofs = 0;
	apu->up_code_buf_sz = config->up_code_buf_sz;
	sec_info->up_code_buf_sz = config->up_code_buf_sz;
	dev_dbg(apu->dev, "%s: up_code_buf_sz is 0x%x\n", __func__, sec_info->up_code_buf_sz);

	sec_info->up_coredump_ofs = sec_info->up_code_buf_ofs + sec_info->up_code_buf_sz +
				    apusys_part_size - MTK_APU_FW_HEADER_OFFSET;
	sec_info->up_coredump_sz = config->up_coredump_buf_sz;
	dev_dbg(apu->dev, "%s: up_coredump_ofs is 0x%x, up_coredump_sz is 0x%x\n",
		__func__, sec_info->up_coredump_ofs, sec_info->up_coredump_sz);

	sec_info->mdla_coredump_ofs = sec_info->up_coredump_ofs + sec_info->up_coredump_sz;
	sec_info->mdla_coredump_sz = config->mdla_coredump_buf_sz;
	dev_dbg(apu->dev, "%s: mdla_coredump_ofs is 0x%x, mdla_coredump_sz is 0x%x\n",
		__func__, sec_info->mdla_coredump_ofs, sec_info->mdla_coredump_sz);

	sec_info->mvpu_coredump_ofs = sec_info->mdla_coredump_ofs + sec_info->mdla_coredump_sz;
	sec_info->mvpu_coredump_sz = config->mvpu_coredump_buf_sz;
	dev_dbg(apu->dev, "%s: mvpu_coredump_ofs is 0x%x, mvpu_coredump_sz is 0x%x\n",
		__func__, sec_info->mvpu_coredump_ofs, sec_info->mvpu_coredump_sz);

	sec_info->mvpu_sec_coredump_ofs = sec_info->mvpu_coredump_ofs + sec_info->mvpu_coredump_sz;
	sec_info->mvpu_sec_coredump_sz = config->mvpu_sec_coredump_buf_sz;
	dev_dbg(apu->dev, "%s: mvpu_sec_coredump_ofs is 0x%x, mvpu_sec_coredump_sz is 0x%x\n",
		__func__, sec_info->mvpu_sec_coredump_ofs, sec_info->mvpu_sec_coredump_sz);

	coredump_buf_sz = sec_info->up_coredump_sz + sec_info->mdla_coredump_sz +
			  sec_info->mvpu_coredump_sz + sec_info->mvpu_sec_coredump_sz +
			  config->ce_coredump_buf_sz;

	sec_mem_sz = roundup(apusys_part_size - MTK_APU_FW_HEADER_OFFSET +
			     sec_info->up_code_buf_sz + coredump_buf_sz,
			     MTK_APUSYS_SEC_MEM_ALIGN);

	sec_info->total_sz = sec_mem_sz;

	return 0;
}

static void mtk_apusys_rv_initialize_sec_buf(struct apusys_secure_info_t *sec_info,
					     void *sec_mem_addr_va)
{
	struct apusys_secure_info_t *sec_info_ptr =
		sec_mem_addr_va + sec_info->up_code_buf_ofs + sec_info->up_code_buf_sz;
	uint32_t offset = sec_info->up_code_buf_ofs;

	sec_info_ptr->total_sz = sec_info->total_sz;

	/* copy uP firmware to code buffer */
	sec_info_ptr->up_code_buf_ofs = offset;
	sec_info_ptr->up_code_buf_sz = sec_info->up_fw_sz;
	memcpy(sec_mem_addr_va + offset, sec_mem_addr_va + sec_info->up_fw_ofs, sec_info->up_fw_sz);
	offset += sec_info->up_code_buf_sz +
		  roundup(sizeof(struct apusys_secure_info_t), APUSYS_FW_ALIGN);

	/* copy MDLA_BOOT firmware */
	sec_info_ptr->mdla_fw_boot_ofs = offset;
	sec_info_ptr->mdla_fw_boot_sz = sec_info->mdla_fw_boot_sz;
	memcpy(sec_mem_addr_va + offset,
	       sec_mem_addr_va + sec_info->mdla_fw_boot_ofs, sec_info->mdla_fw_boot_sz);
	offset += sec_info->mdla_fw_boot_sz;

	/* copy MDLA_MAIN firmware */
	sec_info_ptr->mdla_fw_main_ofs = offset;
	sec_info_ptr->mdla_fw_main_sz = sec_info->mdla_fw_main_sz;
	memcpy(sec_mem_addr_va + offset,
	       sec_mem_addr_va + sec_info->mdla_fw_main_ofs, sec_info->mdla_fw_main_sz);
	offset += sec_info->mdla_fw_main_sz;

	/* copy MVPU firmware */
	sec_info_ptr->mvpu_fw_ofs = offset;
	sec_info_ptr->mvpu_fw_sz = sec_info->mvpu_fw_sz;
	memcpy(sec_mem_addr_va + offset,
	       sec_mem_addr_va + sec_info->mvpu_fw_ofs, sec_info->mvpu_fw_sz);
	offset += sec_info->mvpu_fw_sz;

	/* copy MVPU_SEC firmware */
	sec_info_ptr->mvpu_sec_fw_ofs = offset;
	sec_info_ptr->mvpu_sec_fw_sz = sec_info->mvpu_sec_fw_sz;
	memcpy(sec_mem_addr_va + offset,
	       sec_mem_addr_va + sec_info->mvpu_sec_fw_ofs, sec_info->mvpu_sec_fw_sz);
	offset += sec_info->mvpu_sec_fw_sz;

	/* copy CE firmware */
	sec_info_ptr->ce_bin_ofs = offset;
	sec_info_ptr->ce_bin_sz = sec_info->ce_bin_sz;
	memcpy(sec_mem_addr_va + offset,
	       sec_mem_addr_va + sec_info->ce_bin_ofs, sec_info->ce_bin_sz);
	offset += sec_info->ce_bin_sz;

	sec_info_ptr->up_coredump_ofs = offset;
	sec_info_ptr->up_coredump_sz = sec_info->up_coredump_sz;
	offset += sec_info->up_coredump_sz;

	sec_info_ptr->mdla_coredump_ofs = offset;
	sec_info_ptr->mdla_coredump_sz = sec_info->mdla_coredump_sz;
	offset += sec_info->mdla_coredump_sz;

	sec_info_ptr->mvpu_coredump_ofs = offset;
	sec_info_ptr->mvpu_coredump_sz = sec_info->mvpu_coredump_sz;
	offset += sec_info->mvpu_coredump_sz;

	sec_info_ptr->mvpu_sec_coredump_ofs = offset;
	sec_info_ptr->mvpu_sec_coredump_sz = sec_info->mvpu_sec_coredump_sz;
	offset += sec_info->mvpu_sec_coredump_sz;
}

int mtk_apu_load(struct rproc *rproc, const struct firmware *fw)
{
	int ret = 0;
	struct apusys_secure_info_t *sec_info = NULL;
	unsigned int sec_mem_size = 0;
	size_t apusys_part_size = 0;
	struct mtk_apu *apu = rproc->priv;
	struct mtk_apu_plat_config config = apu->platdata->config;

	/* allocate sec_info memory */
	sec_info = kzalloc(sizeof(struct apusys_secure_info_t), GFP_KERNEL);
	if (!sec_info)
		return -ENOMEM;

	apusys_part_size = fw->size;
	dev_dbg(apu->dev, "%s: apusys_part_size is 0x%zx\n", __func__, apusys_part_size);

	ret = mtk_apusys_rv_fill_sec_info(sec_info, &config, apusys_part_size, apu);
	if (ret) {
		dev_err(apu->dev, "%s: mtk_apusys_rv_fill_sec_info failed: %d\n", __func__, ret);
		goto err_apusys_rv_get_buf_sz;
	}

	ret = mtk_apusys_rv_load_fw(sec_info, apu->resv_mem_va, apu, fw);
	if (ret) {
		dev_err(apu->dev, "%s: mtk_apusys_rv_load_fw failed: %d\n", __func__, ret);
		goto err_apusys_rv_get_buf_sz;
	}

	mtk_apusys_rv_initialize_sec_buf(sec_info, apu->resv_mem_va);

	if (sec_info->ce_bin_sz) {
		ret = mtk_apu_rv_smc_call(apu->dev, MTK_APUSYS_KERNEL_OP_APUSYS_SETUP_CE_BIN, 0);
		if (ret) {
			dev_err(apu->dev, "%s: load ce bin failed, ret = %d\n", __func__, ret);
			goto err_apusys_rv_get_buf_sz;
		}
	}

	/* calc apu aee coredump mem info */
	sec_mem_size = sec_info->total_sz;

	/* set memory addr,size to mtk_apu */
	apu->apusys_sec_mem_start = apu->resv_mem_pa;
	apu->apusys_sec_mem_size = sec_mem_size;
	apu->apu_sec_mem_base = apu->resv_mem_va;
	dev_dbg(apu->dev, "%s: apu_apusys-rv_secure: start = 0x%llx, size = 0x%llx\n",
		__func__, apu->apusys_sec_mem_start, apu->apusys_sec_mem_size);

	kfree(sec_info);

	return ret;

err_apusys_rv_get_buf_sz:
	kfree(sec_info);

	return ret;
}
