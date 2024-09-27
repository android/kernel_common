/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */
#ifndef _SLBC_OPS_H_
#define _SLBC_OPS_H_
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/dma-buf.h>
/* error code */
#define EWAIT_RELEASE		1 /* wait for release */
#define ENOT_AVAILABLE		2 /* not available for now */
#define EREQ_DONE		3 /* already requested */
#define EREQ_MASKED		4 /* req madk bit set */
#define EDISABLED		5 /* req madk bit set */
#define EID_WRONG		6 /* wrong id */
#define EREF_NOT_ZERO		7 /* ref not zero */
#define EREQ_TIMEOUT		8 /* req timeout */
#define EREQ_WAIT_FAIL		9 /* req wait fail */
#define EREQ_GID_RUN_OUT	10 /* req wait fail */
/* call back return value */
#define CB_DONE			0 /* no need to use*/
#define CB_OK			1 /* ready to use/release */
/* slot status */
#define SLOT_AVAILABLE		0 /* slot available*/
#define SLOT_NOT_FOUND		1 /* slot not found */
#define SLOT_USED		2 /* slot used */
/* sid status */
#define SID_NOT_FOUND		0xffff
/* SLC all cache mode magic num */
#define SLC_DATA_MAGIC		0x51ca11ca
/* maximum number of GID */
#define GID_MAX				64
/* GID settings */
#define GS_M	BIT(0)
#define GS_K	BIT(1)
#define GS_RD	BIT(2)
#define GS_FD	BIT(3)
/* need to modify slbc_uid_str  */
enum slbc_uid {
	UID_ZERO = 0,
	UID_MM_VENC,
	UID_MM_DISP,
	UID_MM_MDP,
	UID_MM_VDEC,
	UID_AI_MDLA,
	UID_AI_ISP,
	UID_GPU,
	UID_HIFI3,
	UID_CPU,
	UID_AOV,
	UID_SH_P2,
	UID_SH_APU,
	UID_MML,
	UID_DSC_IDLE,
	UID_AINR,
	UID_TEST_BUFFER, /* 0x10 */
	UID_TEST_CACHE,
	UID_TEST_ACP,
	UID_DISP,
	UID_AOV_DC,
	UID_AOV_APU,
	UID_AISR_APU,
	UID_AISR_MML,
	UID_SH_P1,
	UID_SMT,
	UID_APU,
	UID_AOD,
	UID_BIF,
	UID_MM_VENC_SL,
	UID_SENSOR,
	UID_MM_VENC_FHD,
	UID_MM_VENC_8K,
	UID_MAX,
};
#define UID_MM_BITS_1 (BIT(UID_MM_DISP) | BIT(UID_MM_MDP))
#define BIT_IN_MM_BITS_1(x) ((x) & UID_MM_BITS_1)
#define UID_MM_BITS_2 (BIT(UID_SH_P2) | BIT(UID_SH_APU))
#define BIT_IN_MM_BITS_2(x) ((x) & UID_MM_BITS_2)
#define UID_MM_BITS_3 (BIT(UID_MML) | BIT(UID_DISP))
#define BIT_IN_MM_BITS_3(x) ((x) & UID_MM_BITS_3)
#define UID_MM_BITS_4 (BIT(UID_AISR_APU) | BIT(UID_AISR_MML))
#define BIT_IN_MM_BITS_4(x) ((x) & UID_MM_BITS_4)
enum slbc_type {
	TP_BUFFER = 0,
	TP_CACHE,
	TP_ACP,
};
enum slbc_force {
	FR_DIS = 0,
	FR_CPU,
	FR_GPU,
	FR_APU,
	FR_MAX,
};
#define ACP_ONLY_BIT	2
enum slbc_flag {
	FG_SECURE = BIT(0),
	FG_POWER = BIT(1),
	FG_ACP_ONLY = BIT(ACP_ONLY_BIT),
	FG_ACP_1_4 = BIT(ACP_ONLY_BIT + 1),
	FG_ACP_2_4 = BIT(ACP_ONLY_BIT + 2),
	FG_ACP_3_4 = BIT(ACP_ONLY_BIT + 3),
	FG_ACP_4_4 = BIT(ACP_ONLY_BIT + 4),
};
/* SLC all cache mode user id */
enum slc_ach_uid {
	ID_PD,
	ID_CPU,
	ID_GPU,
	ID_GPU_W,
	ID_OVL_R,
	ID_VDEC_FRAME,
	ID_VDEC_UBE,
	ID_SMMU,
	ID_MD,
	ID_ADSP,
	ID_AOV,
	ID_IMG,
	ID_CAM,
	ID_MAE,
	ID_DMR,
	ID_OD,
	ID_DBI,
	ID_MAX,
};
#define FG_ACP_BITS (FG_ACP_1_4 | FG_ACP_2_4 | FG_ACP_3_4 | FG_ACP_4_4)
#define SLBC_TRY_FLAG_BIT(d, bit) (((d)->flag & (bit)) == (bit))
struct slbc_data {
	unsigned int uid;
	unsigned int type;
	ssize_t size;
	unsigned int flag;
	int ret;
	unsigned int timeout;
	void *user_cb_data;
	/* below used by slbc driver */
	void __iomem *paddr;
	void __iomem *vaddr;
	void __iomem *emi_paddr;

	struct_group(sid_group,
		unsigned int sid;
		unsigned int slot_used;
		void *config;
		int ref;
		int pwr_ref;
		struct slbc_data *private;
	);
};
struct slbc_gid_data {
	unsigned int sign;
	unsigned int buffer_fd;
	unsigned int producer;
	unsigned int consumer;
	unsigned int height;	//unit: pixel
	unsigned int width;	//unit: pixel
	unsigned int dma_size;	//unit: MB
	unsigned int bw;	//unit: MBps
	unsigned int fps;	//unit: frames per sec
	unsigned int flag;
};
#define ui_to_slbc_data(d, ui) \
	do { \
		(d)->uid = ((ui) >> 24 & 0xff); \
		(d)->type = ((ui) >> 16 & 0xff); \
		(d)->flag = ((ui) >> 8 & 0xff); \
	} while (0)
#define slbc_data_to_ui(d) \
	((((d)->uid) & 0xff) << 24 | \
	(((d)->type) & 0xff) << 16 | \
	(((d)->flag) & 0xff) << 8)
struct slbc_ops {
	struct list_head node;
	struct slbc_data *data;
	int (*activate)(struct slbc_data *data);
	int (*deactivate)(struct slbc_data *data);
};
extern unsigned int slbc_enable;
extern unsigned int slbc_all_cache_mode;
extern char *slbc_uid_str[UID_MAX + 1];
extern char *slc_ach_uid_str[ID_MAX + 1];
extern int popcount(unsigned int x);
#if IS_ENABLED(CONFIG_MTK_SLBC)
extern int slbc_status(struct slbc_data *d);
extern int slbc_request(struct slbc_data *d);
extern int slbc_release(struct slbc_data *d);
extern int slbc_power_on(struct slbc_data *d);
extern int slbc_power_off(struct slbc_data *d);
extern int slbc_secure_on(struct slbc_data *d);
extern int slbc_secure_off(struct slbc_data *d);
extern int slbc_register_activate_ops(struct slbc_ops *ops);
extern int slbc_activate_status(struct slbc_data *d);
extern void slbc_update_mm_bw(unsigned int bw);
extern void slbc_update_mic_num(unsigned int num);
extern void slbc_update_inner(unsigned int inner);
extern void slbc_update_outer(unsigned int outer);
void slbc_get_gid_for_dma(struct dma_buf *dmabuf);
int slbc_gid_val(enum slc_ach_uid uid);
int slbc_gid_request(enum slc_ach_uid uid, int *gid, struct slbc_gid_data *data);
int slbc_gid_release(enum slc_ach_uid uid, int gid);
int slbc_roi_update(enum slc_ach_uid uid, int gid, struct slbc_gid_data *data);
int slbc_validate(enum slc_ach_uid uid, int gid);
int slbc_invalidate(enum slc_ach_uid uid, int gid);
int slbc_read_invalidate(enum slc_ach_uid uid, int gid, int enable);
int slbc_force_cache(enum slc_ach_uid uid, unsigned int size);
int slbc_ceil(enum slc_ach_uid uid, unsigned int ceil);
int slbc_window(unsigned int window);
int slbc_get_cache_size(enum slc_ach_uid uid);
int slbc_get_cache_hit_rate(enum slc_ach_uid uid);
int slbc_get_cache_hit_bw(enum slc_ach_uid uid);
int slbc_get_cache_usage(int *cpu, int *gpu, int *other);
#else
__weak int slbc_status(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak int slbc_request(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak int slbc_release(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak int slbc_power_on(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak int slbc_power_off(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak int slbc_secure_on(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak int slbc_secure_off(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak int slbc_register_activate_ops(struct slbc_ops *ops)
{
	return -EDISABLED;
};
__weak int slbc_activate_status(struct slbc_data *d)
{
	return -EDISABLED;
};
__weak void slbc_update_mm_bw(unsigned int bw) {}
__weak void slbc_update_mic_num(unsigned int num) {}
__weak void slbc_update_inner(unsigned int inner) {}
__weak void slbc_update_outer(unsigned int outer) {}
__weak int slbc_gid_val(enum slc_ach_uid uid) { return -EINVAL; }
__weak int slbc_gid_request(enum slc_ach_uid uid, int *gid, struct slbc_gid_data *data)
{
	return -EDISABLED;
};
__weak int slbc_gid_release(enum slc_ach_uid uid, int gid)
{
	return -EDISABLED;
};
__weak int slbc_roi_update(enum slc_ach_uid uid, int gid, struct slbc_gid_data *data)
{
	return -EDISABLED;
};
__weak int slbc_validate(enum slc_ach_uid uid, int gid)
{
	return -EDISABLED;
};
__weak int slbc_invalidate(enum slc_ach_uid uid, int gid)
{
	return -EDISABLED;
};
__weak int slbc_read_invalidate(enum slc_ach_uid uid, int gid, int enable)
{
	return -EDISABLED;
};
__weak int slbc_force_cache(enum slc_ach_uid uid, unsigned int size)
{
	return -EDISABLED;
}
__weak int slbc_ceil(enum slc_ach_uid uid, unsigned int ceil)
{
	return -EDISABLED;
};
__weak int slbc_window(unsigned int window)
{
	return -EDISABLED;
};
__weak int slbc_get_cache_size(enum slc_ach_uid uid)
{
	return -EDISABLED;
};
__weak int slbc_get_cache_hit_rate(enum slc_ach_uid uid)
{
	return -EDISABLED;
};
__weak int slbc_get_cache_hit_bw(enum slc_ach_uid uid)
{
	return -EDISABLED;
}
__weak int slbc_get_cache_usage(int *cpu, int *gpu, int *other)
{
	return -EDISABLED;
}
#endif /* CONFIG_MTK_SLBC */
#endif /* _SLBC_OPS_H_ */
