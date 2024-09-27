// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include "linux/soc/mediatek/apummu_tbl.h"

/*
 * CMU
 *    #define APUMMU_CMU_TOP_BASE 0x19067000, apb register, 1k
 * TCU
 *  RCX
 *    #define APUMMU_RCX_UPRV_TCU_BASE  0x19060000
 *    #define APUMMU_RCX_EXTM_TCU_BASE  0x19061000
 *    #define APUMMU_RCX_EDPA_TCU_BASE  0x19062000
 *    #define APUMMU_RCX_MDLA_TCU_BASE  0x19063000
 */

#ifdef linux_ep
void *ammu_cmu_top_base;
void *ammu_rcx_uprv_tcu_base;
void *ammu_rcx_extm_tcu_base;
#ifdef SMMU_EN
void *ammu_vcore_config_base;
#endif

int apummu_ioremap(void)
{
	/* ammu_cmu_top_base	= ioremap(APUMMU_CMU_TOP_BASE, 0x5000); */
	ammu_cmu_top_base	= ioremap(APUMMU_CMU_TOP_REG_BASE, 0x7000);
	ammu_rcx_uprv_tcu_base = ioremap(APUMMU_RCX_UPRV_TCU_REG_BASE, 0x1000);
	ammu_rcx_extm_tcu_base = ioremap(APUMMU_RCX_EXTM_TCU_REG_BASE, 0x1000);
#ifdef SMMU_EN
	ammu_vcore_config_base = ioremap(APUMMU_VCORE_CONFIG_REGISTER, 0x4);

	printf("ammu remap: CMU(0x%llx), RV TCU(0x%llx), EXTM TCU(0x%llx), SMMU CFG(0x%llx)\n",
		(uint64_t)ammu_cmu_top_base,
		(uint64_t)ammu_rcx_uprv_tcu_base,
		(uint64_t)ammu_rcx_extm_tcu_base,
		(uint64_t)ammu_vcore_config_base);
#else
	printf("ammu remap adr: CMUL(0x%llx), RV TCU(0x%llx) , EXTM TCU(0x%llx)\n",
		(uint64_t)ammu_cmu_top_base,
		(uint64_t)ammu_rcx_uprv_tcu_base,
		(uint64_t)ammu_rcx_extm_tcu_base);
#endif

	return 0;
}
#else /* non linux ep */

static const mmap_region_t apusys_ammu_mmap[] MTK_MMAP_SECTION = {
	MAP_REGION_FLAT(APU_CMU_TOP, APU_CMU_TOP_SZ,
		MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(APU_RCX_UPRV_TCU, APU_RCX_UPRV_TCU_SZ,
		MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(APU_RCX_EXTM_TCU, APU_RCX_EXTM_TCU_SZ,
		MT_DEVICE | MT_RW | MT_SECURE),
	{0}
};
DECLARE_MTK_MMAP_REGIONS(apusys_ammu_mmap);
#endif /* end ep */


/* Get vsid segment 0x00 */
int apummu_get_segment_offset0(uint32_t vsid_idx, uint8_t seg_idx, uint32_t *input_adr,
					uint8_t *res_bits, uint8_t *page_sel, uint8_t *page_len)
{
	//check abnormal parameter
	//printf parameters
	*input_adr = APUMMU_SEGMENT_GET_INPUT(vsid_idx, seg_idx);
	*res_bits  = APUMMU_SEGMENT_GET_OFFSET0_RSRV(vsid_idx, seg_idx);
	*page_sel  = APUMMU_SEGMENT_GET_PAGESEL(vsid_idx, seg_idx);
	*page_len  = APUMMU_SEGMENT_GET_PAGELEN(vsid_idx, seg_idx);

	return 0; //indicate success or fail
}

/* Get vsid segment 0x04 */
void apummu_get_segment_offset1(uint32_t vsid_idx, uint8_t seg_idx,
				uint32_t *output_adr, uint8_t *res_bits0,
				uint8_t *SMMU_en, uint8_t *res_bits1)
{
	*output_adr = APUMMU_SEGMENT_GET_OUTPUT(vsid_idx, seg_idx);
	*res_bits0  = APUMMU_SEGMENT_GET_OFFSET1_RSRV0(vsid_idx, seg_idx);
	*SMMU_en   = APUMMU_SEGMENT_GET_MMU_EN(vsid_idx, seg_idx);
	*res_bits1  = APUMMU_SEGMENT_GET_OFFSET1_RSRV1(vsid_idx, seg_idx);
}

/* Get vsid segment 0x08 */
void apummu_get_segment_offset2(uint32_t vsid_idx, uint8_t seg_idx,
				uint8_t *domain, uint8_t *acp_en,
				uint8_t *aw_clr, uint8_t *aw_invalid,
				uint8_t *ar_exclu, uint8_t *ar_sepcu,
				uint8_t *aw_cache_alloc, uint8_t *aw_slc_en,
				uint8_t *aw_slb_en, uint8_t *ar_cache_alloc,
				uint8_t *ar_slc_en, uint8_t *ar_slb_en,
				uint8_t *ns)
{
	*domain = APUMMU_SEGMENT_GET_DOMAIN(vsid_idx, seg_idx);
	*acp_en = APUMMU_SEGMENT_GET_ACP_EN(vsid_idx, seg_idx);
	*aw_clr = APUMMU_SEGMENT_GET_AW_CLR(vsid_idx, seg_idx);
	*aw_invalid = APUMMU_SEGMENT_GET_AW_INVALID(vsid_idx, seg_idx);
	*ar_exclu = APUMMU_SEGMENT_GET_AR_EXCLU(vsid_idx, seg_idx);
	*ar_sepcu = APUMMU_SEGMENT_GET_AR_SEPCU(vsid_idx, seg_idx);
	*aw_cache_alloc = APUMMU_SEGMENT_GET_AW_CACHE_ALLOC(vsid_idx, seg_idx);
	*aw_slc_en = APUMMU_SEGMENT_GET_AW_SLC_EN(vsid_idx, seg_idx);
	*aw_slb_en = APUMMU_SEGMENT_GET_AW_SLB_EN(vsid_idx, seg_idx);
	*ar_cache_alloc = APUMMU_SEGMENT_GET_AR_CACHE_ALLOC(vsid_idx, seg_idx);
	*ar_slc_en = APUMMU_SEGMENT_GET_AR_SLC_EN(vsid_idx, seg_idx);
	*ar_slb_en = APUMMU_SEGMENT_GET_AR_SLB_EN(vsid_idx, seg_idx);
	*ns = APUMMU_SEGMENT_GET_NS(vsid_idx, seg_idx);
}

void apummu_get_segment_offset3(uint32_t vsid_idx, uint8_t seg_idx,
			       uint8_t *seg_valid)
{
	*seg_valid = APUMMU_SEGMENT_GET_SEG_VALID(vsid_idx, seg_idx);
}


/* Set vsid segment 0x00 */
void apummu_set_segment_offset0(uint32_t vsid_idx, uint8_t seg_idx, uint32_t input_adr,
					uint8_t res_bits, uint8_t page_sel, uint8_t page_len)
{
	/* check abnormal parameter
	 * printf parameters
	 */
	DRV_WriteReg32(APUMMU_VSID_SEGMENT_BASE(vsid_idx, seg_idx, 0),
		APUMMU_BUILD_SEGMENT_OFFSET0(input_adr, res_bits, page_sel, page_len));
}


/* Set vsid segment 0x04 */
void apummu_set_segment_offset1(uint32_t vsid_idx, uint8_t seg_idx, uint32_t output_adr,
					uint8_t res0, uint8_t smmu_en, uint8_t res1)
{
	/* check abnormal parameter
	 * printf parameters
	 */
	DRV_WriteReg32(APUMMU_VSID_SEGMENT_BASE(vsid_idx, seg_idx, 1),
				APUMMU_BUILD_SEGMENT_OFFSET1(output_adr, res0, smmu_en, res1));
}


/* set vsid segment 0x08 -prefer bit fields as input parameters */
void apummu_set_segment_offset2(
	uint32_t vsid_idx, uint8_t seg_idx, uint8_t resv, uint8_t domain, uint8_t acp_en,
	uint8_t aw_clr, uint8_t aw_invalid, uint8_t ar_exclu, uint8_t ar_sepcu,
	uint8_t aw_cache_allocate, uint8_t aw_slc_en, uint8_t aw_slb_en,
	uint8_t ar_cache_allocate, uint8_t ar_slc_en, uint8_t ar_slb_en, uint8_t ro, uint8_t ns)
{
	DRV_WriteReg32(APUMMU_VSID_SEGMENT_BASE(vsid_idx, seg_idx, 2),
			APUMMU_BUILD_SEGMENT_OFFSET2(
				resv, domain, acp_en, aw_clr, aw_invalid,
				ar_exclu, ar_sepcu, aw_cache_allocate,
				aw_slc_en, aw_slb_en, ar_cache_allocate,
				ar_slc_en, ar_slb_en, ro, ns));
}


/* set vsid segment 0x0c */
void apummu_set_segment_offset3(uint32_t vsid_idx, uint8_t seg_idx, uint8_t seg_valid, uint8_t rsv)
{
	DRV_WriteReg32(APUMMU_VSID_SEGMENT_EN_BASE(vsid_idx),
		(DRV_Reg32(APUMMU_VSID_SEGMENT_EN_BASE(vsid_idx)) | (1 << seg_idx)));
}


/*
 * apummu_enable_vsid()

 * VSID in CMU Configuration

 * 0x50 VSID_enable0_set 32 1  VSID enable0 set BX; SCU;
 * RAND 31 0 the_VSID31_0_enable_set_register

 * 0x54 VSID_enable1_set 32 1  VSID enable1 set BX; SCU;
 * RAND 31 0 the_VSID63_32_enable_set_register

 * *((UINT32P)(APU_RCX_AMU_CMU_TOP+(vsid/32)*0x4+0x50)) = 0x1 << (vsid%32);
 * *((UINT32P)(APU_RCX_AMU_CMU_TOP+(vsid/32)*0x4+0xb0)) = 0x1 << (vsid%32);

*/
int apummu_enable_vsid(uint32_t vsid_idx)
{
	if (vsid_idx > (APUMMU_VSID_ACTIVE-1) &&
			vsid_idx < (APUMMU_RSV_VSID_IDX_END - APUMMU_VSID_RSV+1)) {
		printf("invalid vsid index %d\n", vsid_idx);
		return -1;
	}

	/* this vsid is distributed */
	DRV_WriteReg32(APUMMU_VSID_ENABLE_BASE(vsid_idx), 0x1 << (vsid_idx & 0x1f));
	/* this vsid is ready for used */
	DRV_WriteReg32(APUMMU_VSID_VALID_BASE(vsid_idx),  0x1 << (vsid_idx & 0x1f));

	return 0;
}

/*
 * apummu_enable()
 * CMU init
 * ((UINT32P)(0x19067000)) = 0x1; //bit0: apu_mmu_en
 * 0x0	cmu_con	32	1	cmu control register
 *				0	0	APU_MMU_enable
 *				1	1	tcu_apb_dbg_en
 *				2	2	tcu_secure_chk_en
 *				3	3	DCM_en
 *				4	4	sw_slp_prot_en_override
 */
void apummu_enable(void)
{
	uint32_t flag = 0;

	/* need to read first, only set bit 0 (keep the default value) */
	flag = DRV_Reg32(APUMMU_CMU_TOP_BASE);
	flag |= 0x1;
	DRV_WriteReg32(APUMMU_CMU_TOP_BASE, flag);
}

int apummu_topology_init(void)
{
	/* Note: This is don't care since no invalidate is called */
	DRV_WriteReg32(APUMMU_CMU_TOP_TOPOLOGY, ((0xf << 7) | 0x03));

	return 0;
}

/* apummu_vsid_sram_config() @ drv_init */
int apummu_vsid_sram_config(void)
{
	uint32_t idx;

	for (idx = 0; idx < APUMMU_VSID_RSV; idx++) {
		DRV_WriteReg32(APUMMU_VSID(APUMMU_RSV_VSID_IDX_START+idx),
			APUMMU_VSID_DESC(APUMMU_VSID_SRAM_TOTAL - APUMMU_VSID_RSV + idx));
	}


#ifdef SHOW_COMMENT
	/* config active vsid's vsid desc 0-31 (ponsot) */
	for (idx = 0; idx < APUMMU_VSID_ACTIVE; idx++)
		DRV_WriteReg32(APUMMU_VSID(idx), APUMMU_VSID_DESC(idx));

	/* set valid bit = 0 of 10 segment to avoid abnormal operations */
	/* TBD */
#endif

	return 0;
}

/* apummu_uP_monitor */
void apummu_rv_addr_monitor(uint32_t lower, uint32_t upper)
{
	printf("%s: lower=0x%x, upper=0x%x\n", __func__,
	     lower, upper);

	DRV_WriteReg32((APUMMU_RCX_UPRV_TCU_BASE + (0xE60)), lower);
	DRV_WriteReg32((APUMMU_RCX_UPRV_TCU_BASE + (0xE64)), upper);
	DRV_WriteReg32((APUMMU_RCX_UPRV_TCU_BASE + (0xE68)), ((0<<2) | (0<<0)));
	DRV_WriteReg32((APUMMU_RCX_UPRV_TCU_BASE + (0xE6C)), ((0<<1)));
	DRV_WriteReg32((APUMMU_RCX_UPRV_TCU_BASE + (0xE40)), ((1<<3)));
}

/* apummu_uP_monitor latch */
uint32_t apummu_rv_monitor_latch(void)
{
	uint32_t status = 0;

	status = DRV_Reg32((APUMMU_RCX_UPRV_TCU_BASE + (0xEA0)));
	printf("apummu_rv_monitor_latch_(ea0): status = 0x%x\n", status);

	return DRV_Reg32((APUMMU_RCX_UPRV_TCU_BASE + (0xE7C)));
}

/*
 * apummu_bind_vsid()
 * //thread mapping to VSID table
 * e.g
 * //bit17 to bit11: COR_ID, bit10 to bit3: VSID, bit0: VSID valid, bit1: COR_ID valid
 * *((UINT32P)(base + (thread*0x4))) =  (cor_id << 11) + (vsid << 3) + 0x1 + 0x2;
 *   => VSDI valid:1 -> means the enigne bind this visd
 *   => VSID valid:0 -> interurpt will be triggered if transaction is sent
 * 17	11	cor_id
 * 10	3	vsid
 * 2	2	vsid_prefetch_trigger
 * 1	1	corid_vld
 * 0	0	vsid_vld
 */
#ifdef linux_ep
void apummu_bind_vsid(void *tcu_base, uint32_t vsid_idx, uint8_t cor_id, uint8_t hw_thread,
					uint8_t cor_valid, uint8_t vsid_valid)
#else
void apummu_bind_vsid(uint32_t tcu_base, uint32_t vsid_idx, uint8_t cor_id, uint8_t hw_thread,
					uint8_t cor_valid, uint8_t vsid_valid)
#endif
{
#ifdef linux_ep
	printf("TCU BASE:0x%llx, vsid=%d, core_id=%d, thread=%d, cor_valid=%d, vsid_valid=%d\n",
			(uint64_t)tcu_base, vsid_idx, cor_id, hw_thread, cor_valid, vsid_valid);
#else
	printf("TCU BASE:0x%x, vsid=%d, core_id=%d, thread=%d, cor_valid=%d, vsid_valid=%d\n",
			tcu_base, vsid_idx, cor_id, hw_thread, cor_valid, vsid_valid);
#endif
	DRV_WriteReg32((tcu_base + hw_thread*0x4), (((cor_id & 0x7f) << 11)
				| ((vsid_idx & 0xff) << 3)
				| ((cor_valid & 0x1) << 1) | ((vsid_valid & 0x1) << 0)));
}

int apummu_rv_bind_vsid(uint8_t hw_thread)
{
	//UINT32 thread	= 2;//md32 user->thread0, logger -> thread2 ?
	if (hw_thread > 7) {
		printf("the hw thread id (%d) is not valid for rv/logger\n", hw_thread);
		return -1;
	}

	/* for rV */
	apummu_bind_vsid(APUMMU_RCX_UPRV_TCU_BASE, APUMMU_UPRV_RSV_VSID, 0, 0, 0, 1);
	apummu_bind_vsid(APUMMU_RCX_UPRV_TCU_BASE, APUMMU_UPRV_RSV_VSID, 0, hw_thread, 0, 1);

	printf("Binding APUMMU_UPRV_RSV_VSID successfully (%d)\n", APUMMU_UPRV_RSV_VSID);

	return 0;
}

int apummu_logger_bind_vsid(uint8_t hw_thread)
{

	if (hw_thread > 7) {
		printf("the hw thread id (%d) is not valid for rv/logger\n", hw_thread);
		return -1;
	}
	/* for logger */
	apummu_bind_vsid(APUMMU_RCX_UPRV_TCU_BASE, APUMMU_LOGGER_RSV_VSID, 0, hw_thread, 0, 1);

	printf("Binding APUMMU_LOGGER_RSV_VSID successfully (%d)\n", APUMMU_LOGGER_RSV_VSID);

	return 0;
}

int apummu_apmcu_bind_vsid(uint8_t hw_thread)
{

	if (hw_thread > 7) {
		printf("the hw thread id (%d) is not valid for rv/logger\n", hw_thread);
		return -1;
	}
	/* for APmcu */
	apummu_bind_vsid(APUMMU_RCX_EXTM_TCU_BASE, APUMMU_APMCU_RSV_VSID, 0, hw_thread, 0, 1);

	printf("Binding APUMMU_LOGGER_RSV_VSID successfully (%d)\n", APUMMU_APMCU_RSV_VSID);

	return 0;
}


/*
 * apummu_add_map()

 * vsid_idx:0-255
 * seg_idx :0-9
 * input eva: 22bits
 * output remap_adr: 22bits

 * Page length:This field indicate the segment size selection. There are several options
 * page length=0-> size 128KB
 * page length=1-> size 256KB
 * page length=2-> size 512KB
 * page length=3-> size 1MB
 * page length=4-> size 128MB
 * page length=5-> size 256MB
 * page length=6-> size 512MB
 * page length=7-> size 4GB
 * when page sel=0, total remap size = input base+page length (page enable bit ?¡æ?)
 * when page sel=3~7, total remap size=input base+page length*page enable

 * Segment layout for reserved VSID
 * reserved VSID 254 for uP
 * reserved VSID 253 for logger
 * reserved VSID 252 for ARM
 * reserved VSID 251 for GPU
 * reserved VSID 250 for sAPU
 * reserved VSID 249 for AoV (1-1 mapping, or disable APUMMMU)

 * Up
 * seg_idx 0  - 1M
 * seg_idx 1  - 512M
 * seg_idx 2  - 4G
 * e.g	apummu_add_map(254, 0, 0, output_adr, 0, 3, 5, 0);
 *	apummu_add_map(254, 1, 0, output_adr, 0, 6, 5, 0);
 *	apummu_add_map(254, 2, 0, output_adr, 0, 7, 7, 1);
 * Logger
 * seg_idx 0  - 4G

 * xPU (CPU/GPU)
 * seg_idx 0 - core dump address
 */
#ifndef linux_ep
apummu_vsid_t gApummu_vsid;
#endif
int apummu_add_map(uint32_t vsid_idx, uint8_t seg_idx, uint32_t input_adr, uint32_t output_adr,
			uint8_t page_sel, uint8_t page_len, uint8_t domain, uint8_t ns)
{
#ifdef SMMU_EN
	uint8_t sid, smmu_en;
#endif

#ifdef COMMENT_SHOW
	if ((input_adr & 0x3fffff) != 0 || (output_adr & 0x3fffff) != 0) { // check 4k alignment
		printf("input/output adr is not 4k alignment (%x / %x)\n", input_adr, output_adr);
		return -1; //error code
	}
#endif
	/* if (vsid_idx > (APUMMU_VSID_ACTIVE-1) && \
	 *        vsid_idx < (APUMMU_RSV_VSID_IDX_END - APUMMU_VSID_RSV+1)) { //check vsid if valid
	 *        printf("vsid_idx  is not valid  (0x%x:0x%x)\n", vsid_idx);
	 *   return -2;
	 * }
	 */

	if (seg_idx > 9) { /* check segment position if illegal */
		printf("seg_idx  is not illegal (0x%x)\n", seg_idx);
		return -3;
	}

#ifdef SMMU_EN
#ifdef FPGA
	sid = 0;
#else
	sid = 9;
#endif
	smmu_en = 1;
#endif

	/* fill segment */
	apummu_set_segment_offset0(vsid_idx, seg_idx, input_adr, 0, page_sel, page_len);
#ifdef SMMU_EN
	apummu_set_segment_offset1(vsid_idx, seg_idx, output_adr, sid, smmu_en, 0);
#else
	apummu_set_segment_offset1(vsid_idx, seg_idx, output_adr, 0, 1, 0);
#endif
	apummu_set_segment_offset2(vsid_idx, seg_idx, 0, domain,
					0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ns);
	apummu_set_segment_offset3(vsid_idx, seg_idx, 1, 0);

#ifdef COMMENT_SHOW /* ndef linux_ep */
	/* fill page array if needs */
#endif

	return 0; /* success or fail */
}

void apummu_dump_map(uint32_t vsid_idx, uint8_t seg_idx)
{
	uint32_t input_adr = 0, output_adr = 0;
	uint8_t resv = 0, smmu_sid = 0, axmmusecsid = 0;
	uint8_t page_sel = 0, page_len = 0;
	uint8_t en = 0;
	uint8_t domain = 0, acp_en = 0, aw_clr = 0;
	uint8_t aw_invalid = 0, ar_exclu = 0;
	uint8_t ar_sepcu = 0, aw_cache_alloc = 0;
	uint8_t aw_slc_en = 0, aw_slb_en = 0;
	uint8_t ar_cache_alloc = 0, ar_slc_en = 0;
	uint8_t ar_slb_en = 0, ns = 0;
	uint8_t seg_valid = 0;

	apummu_get_segment_offset0(vsid_idx, seg_idx, &input_adr,
				   &resv, &page_sel, &page_len);
	apummu_get_segment_offset1(vsid_idx, seg_idx, &output_adr,
				   &smmu_sid, &en, &axmmusecsid);
	apummu_get_segment_offset2(vsid_idx, seg_idx, &domain, &acp_en,
				   &aw_clr, &aw_invalid, &ar_exclu,
				   &ar_sepcu, &aw_cache_alloc, &aw_slc_en,
				   &aw_slb_en, &ar_cache_alloc, &ar_slc_en,
				   &ar_slb_en, &ns);
	apummu_get_segment_offset3(vsid_idx, seg_idx, &seg_valid);

	printf("----vsid = 0x%x, seg_idx = 0x%x-----\n", vsid_idx, seg_idx);

	printf("input_adr = 0x%x, output_adr = 0x%x\n", input_adr, output_adr);
	printf("smmu_sid = 0x%x, ssid_en = 0x%x, axmmusecsid = 0x%x\n",
		smmu_sid, en, axmmusecsid);
	printf("resv = 0x%x, page_sel = 0x%x, page_len = 0x%x\n",
	     resv, page_sel, page_len);

	printf("domain = 0x%x, ns= 0x%x\n", domain, ns);
	printf("acp_en = 0x%x, aw_clr = 0x%x, aw_invalid = 0x%x\n",
	     acp_en, aw_clr, aw_invalid);
	printf("ar_exclu = 0x%x, ar_sepcu = 0x%x, aw_cache_alloc = 0x%x\n",
	     ar_exclu, ar_sepcu, aw_cache_alloc);
	printf("aw_slc_en = 0x%x, aw_slb_en = 0x%x\n", aw_slc_en, aw_slb_en);
	printf("ar_cache_alloc = 0x%x, ar_slc_en = 0x%x, ar_slb_en = 0x%x\n",
	     ar_cache_alloc, ar_slc_en, ar_slb_en);

	printf("---------seg_valid = 0x%x---------\n", seg_valid);

}

int apummu_boot_init(void)
{

#ifdef linux_ep
	/* apummu ioremap */
	apummu_ioremap();
#endif
	/* vsid sram descript init */
	apummu_vsid_sram_config();

#ifdef COMMENT_SHOW /* ndef linux_ep */
	/* topology init */
	apummu_topology_init();
#endif

#ifdef SMMU_EN
	/* Set SID (8), SSID(0) */
	DRV_WriteReg32(ammu_vcore_config_base, ((0x8 << 4) | (0x0 << 0)));
#endif
	/* enable apummu h/w */
	apummu_enable();


	return 0;
}


/*
 * virtual engine thread generator
 */

void virtual_engine_thread(void)
{
	uint32_t thread_map;
	uint32_t data;

/*
 * init TEE dns 0 maps thread 1
 * (dns consists of 5 bits xxxy -> xxxx is domain, y is ns)
 */
#ifdef linux_ep
	thread_map = APUMMU_THD_ID_APMCU_NORMAL; //0
	data = (thread_map << 3);
#else
	thread_map = APUMMU_THD_ID_TEE; //1
	data = thread_map;
#endif

	DRV_WriteReg32((APUMMU_RCX_EXTM_TCU_BASE + APUMMU_INT_D2T_TBL0_OFS),
		       data);
}

/*
 * for apmcu map
 */
int apummu_add_apmcu_map(uint32_t seg_input0, uint32_t seg_output0, enum eAPUMMUPAGESIZE page_size)
{
	int ret = 0;
	uint8_t domain = 7, ns = 1;

	DRV_WriteReg32(APUMMU_VSID_SEGMENT_EN_BASE(APUMMU_APMCU_RSV_DESC_IDX), 0);

	/* must be in order */
	ret = apummu_add_map(APUMMU_APMCU_RSV_DESC_IDX, 0, seg_input0, seg_output0,
				0, page_size, domain, ns); /* page length=3 -> size 1MB */
	if (ret)
		return ret;

	/* enable vsid */
	ret = apummu_enable_vsid(APUMMU_APMCU_RSV_VSID);

	return 0;
}


/*
 * for logger map
 */
int apummu_add_logger_map(uint32_t seg_input0, uint32_t seg_output0, enum eAPUMMUPAGESIZE page_size)
{
	int ret = 0;
	uint8_t domain = 7, ns = 1;

	// DRV_WriteReg32(APUMMU_VSID_SEGMENT_EN_BASE(APUMMU_LOGGER_RSV_DESC_IDX), 0);

	/* must be in order */
	ret = apummu_add_map(APUMMU_LOGGER_RSV_DESC_IDX, 0, seg_input0, seg_output0,
				0, page_size, domain, ns); /* page length=3 -> size 1MB */
	if (ret)
		return ret;

	/* enable vsid */
	ret = apummu_enable_vsid(APUMMU_LOGGER_RSV_VSID);

	return ret;
}

/*
 * for rv boot map
 */
int apummu_add_rv_boot_map(uint32_t seg_output0, int32_t seg_output1, int32_t seg_output2)
{
	int ret = 0;
	uint8_t domain = 7, ns = 1;

	DRV_WriteReg32(APUMMU_VSID_SEGMENT_EN_BASE(APUMMU_RSV_VSID_DESC_IDX_END), 0);

	/* must be in order */
	/* eAPUMMU_PAGE_LEN_1MB, = 3 */
	ret = apummu_add_map(APUMMU_RSV_VSID_DESC_IDX_END, 0, 0, seg_output0,
				0, eAPUMMU_PAGE_LEN_1MB, domain, ns);
	/* eAPUMMU_PAGE_LEN_512MB, 6 */
	ret |= apummu_add_map(APUMMU_RSV_VSID_DESC_IDX_END, 1, 0, seg_output1,
				0, eAPUMMU_PAGE_LEN_512MB, domain, ns);
	/* eAPUMMU_PAGE_LEN_4GB, 7 */
	ret |= apummu_add_map(APUMMU_RSV_VSID_DESC_IDX_END, 2, 0, seg_output2,
				0, eAPUMMU_PAGE_LEN_4GB, domain, ns);

	/* enable vsid */
	ret |= apummu_enable_vsid(APUMMU_UPRV_RSV_VSID);

	return ret;
}

/* Example func. Before RCX power on */
int rv_boot(uint32_t uP_seg_output, uint8_t uP_hw_thread,
		uint32_t logger_seg_output, enum eAPUMMUPAGESIZE logger_page_size,
		uint32_t XPU_seg_output, enum eAPUMMUPAGESIZE XPU_page_size)
{
	int ret = 0;

	/* apummu init @ beginning - call this once only */
	apummu_boot_init();
	printf("<%s> output addr, uP_seg = 0x%8x logger_seg = 0x%8x XPU_seg = 0x%8x\n",
		 __func__, uP_seg_output, logger_seg_output, XPU_seg_output);
	printf("<%s> uP_hw_thread = %u, (logger, XPU) page size = (%u, %u)\n",
		 __func__, uP_hw_thread, logger_page_size, XPU_page_size);

	/* 1. add rv map - MUST be in-order for rv booting */
	ret = apummu_add_rv_boot_map(uP_seg_output, 0, 0);
	if (ret) {
		printf("apummu_add_rv_boot_map fail\n");
		return ret;
	}

	/* bind rv vsid */
	/* thread: 0:normal, 1:secure, 2:logger?; MP flow should be 1 */
	ret = apummu_rv_bind_vsid(uP_hw_thread);
	if (ret) {
		printf("apummu_rv_bind_vsid fail(%u)\n", uP_hw_thread);
		return ret;
	}

	ret = apummu_rv_bind_vsid(uP_hw_thread + 1);
	if (ret) {
		printf("apummu_rv_bind_vsid fail(%u)\n", (uP_hw_thread + 1));
		return ret;
	}

	/* 2.  add h/w logger map */
	ret = apummu_add_logger_map(logger_seg_output/*input*/, logger_seg_output/*output*/,
							logger_page_size);
	if (ret) {
		printf("apummu add map for logger fail\n");
		return ret;
	}

	/* bind logger vsid */
	ret = apummu_logger_bind_vsid(uP_hw_thread+2); //thread: 0:normal, 1:secure, 2:logger
	if (ret) {
		printf("apummu_logger_bind_vsid fail(%u)\n", (uP_hw_thread + 2));
		return ret;
	}

	/* 3. add apmcu map */
	virtual_engine_thread();
	ret = apummu_add_apmcu_map(XPU_seg_output/*input*/, XPU_seg_output/*output*/,
							XPU_page_size);
	if (ret) {
		printf("apummu add map for apmcu fail\n");
		return ret;
	}

	/* bind apmmu vsid */
	ret = apummu_apmcu_bind_vsid(APUMMU_THD_ID_APMCU_NORMAL);
	if (ret) {
		printf("apummu_apmcu_bind_vsid fail\n");
		return ret;
	}

	return ret;
}
/* Example func. */
