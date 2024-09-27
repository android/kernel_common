/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef __MTK_APU_MDW_H__
#define __MTK_APU_MDW_H__

#include <linux/miscdevice.h>
#include <linux/iopoll.h>
#include <linux/irqreturn.h>
#include <linux/dma-fence.h>
#include <linux/of_device.h>
#include <linux/dma-direction.h>
#include <linux/scatterlist.h>
#include <linux/hashtable.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/dma-fence.h>
#include <linux/hashtable.h>
#include <linux/genalloc.h>
#include <linux/min_heap.h>

#include "linux/soc/mediatek/mtk_apu_device.h"
#include "mdw_ioctl.h"
#include "mdw_import.h"

#define MDW_NAME "apusys"
#define MDW_DEV_MAX (APUSYS_DEVICE_MAX)
#define MDW_DEV_TAB_DEV_MAX (16)
#define MDW_CMD_MAX (32)
#define MDW_SUBCMD_MAX (64)
#define MDW_PRIORITY_MAX (32)
#define MDW_DEFAULT_TIMEOUT_MS (30*1000)
#define MDW_BOOST_MAX (100)
#define MDW_DEFAULT_ALIGN (16)
#define MDW_UTIL_CMD_MAX_SIZE (1*1024*1024)

#define MDW_FENCE_MAX_RINGS (64)

#define MDW_ALIGN(x, align) ((x+align-1) & (~(align-1)))

/* history parameter */
#define MDW_NUM_HISTORY 2
#define MDW_NUM_PREDICT_CMD 16
#define MDW_POWER_GAIN_TH 7680 //us
#define MDW_PERIOD_TOLERANCE_TH(x) div_u64((x) * 10, 100) //ms
#define MDW_IPTIME_TOLERANCE_TH(x) div_u64((x) * 10, 100) //ms
#define MDW_EXECTIME_TOLERANCE_TH(x) div_u64((x) * 50, 100) //us

/* power budget debounce time */
#define MDW_PB_DEBOUNCE_MS (50*1000)

/* dtime */
#define MAX_DTIME (2000) /* 2s */

struct mdw_fpriv;
struct mdw_device;
struct mdw_mem;

extern const struct mdw_plat_func rv_plat_drv_v2;
extern const struct mdw_plat_func rv_plat_drv_v3;
extern const struct mdw_plat_func rv_plat_drv_v4;

enum mdw_perf_cmd_state {
	MDW_PERF_CMD_INIT,
	MDW_PERF_CMD_DONE,
};

enum mdw_power_type {
	MDW_APU_POWER_OFF,
	MDW_APU_POWER_ON,
};

enum mdw_buf_type {
	MDW_DATA_BUF,
	MDW_CMD_BUF,
};

enum mdw_mem_op {
	MDW_MEM_OP_NONE,
	MDW_MEM_OP_INTERNAL,
	MDW_MEM_OP_ALLOC,
	MDW_MEM_OP_IMPORT,
};

struct mdw_mem_map {
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct kref map_ref;
	struct mdw_mem *m;
	void (*get)(struct mdw_mem_map *map);
	void (*put)(struct mdw_mem_map *map);
};

struct mdw_mem_invoke {
	struct list_head map_node; //to mdw_mem_map
	struct list_head u_node; //to mpriv
	struct kref ref;
	struct mdw_mem *m;
	struct mdw_fpriv *invoker;
	void (*get)(struct mdw_mem_invoke *m_invoke);
	void (*put)(struct mdw_mem_invoke *m_invoke);
};

enum mdw_queue_type {
	MDW_QUEUE_COMMON,
	MDW_QUEUE_NORMAL,
	MDW_QUEUE_DEADLINE,

	MDW_QUEUE_MAX,
};

struct mdw_mem {
	/* in */
	enum mdw_mem_type type;
	uint64_t size;
	uint64_t align;
	uint64_t flags;
	uint32_t buf_type;

	/* out */
	void *vaddr;
	uint32_t tbl_daddr;
	struct device *mdev;
	struct dma_buf *dbuf;
	void *priv;
	int (*bind)(void *session, struct mdw_mem *m);
	void (*unbind)(void *session, struct mdw_mem *m);

	/* map */
	uint64_t device_va;
	uint64_t device_iova;
	uint64_t dva_size;
	struct mdw_mem_map *map;

	/* control */
	int handle;
	bool belong_apu;
	bool need_handle;
	struct list_head maps;
	struct mdw_fpriv *mpriv;
	struct mdw_mem_pool *pool;
	struct list_head u_item; //to mpriv
	struct list_head d_node; //to mdev
	struct list_head p_chunk; //to mem pool
	struct mutex mtx;
	void (*release)(struct mdw_mem *m);
};

/* default chunk size of memory pool */
#define MDW_MEM_POOL_CHUNK_SIZE (4*1024*1024)

struct mdw_mem_pool {
	struct mdw_fpriv *mpriv;
	/* pool attribute */
	enum mdw_mem_type type;
	uint64_t flags;
	uint32_t align;
	uint32_t chunk_size;
	/* container and lock */
	struct gen_pool *gp;
	struct mutex m_mtx;
	/* list of resource chunks */
	struct list_head m_chunks;
	/* list of allocated memories from gp */
	struct list_head m_list;
	/* ref count for cmd/mem */
	struct kref m_ref;
	void (*get)(struct mdw_mem_pool *pool);
	void (*put)(struct mdw_mem_pool *pool);
};

struct mdw_dinfo {
	uint32_t type;
	uint32_t num;
	uint8_t meta[MDW_DEV_META_SIZE];
};

enum mdw_driver_type {
	MDW_DRIVER_TYPE_PLATFORM,
	MDW_DRIVER_TYPE_RPMSG,
};

enum mdw_info_type {
	MDW_INFO_KLOG,
	MDW_INFO_ULOG,
	MDW_INFO_PREEMPT_POLICY,
	MDW_INFO_SCHED_POLICY,

	MDW_INFO_NORMAL_TASK_DLA,
	MDW_INFO_NORMAL_TASK_DSP,
	MDW_INFO_NORMAL_TASK_DMA,

	MDW_INFO_MIN_DTIME,
	MDW_INFO_MIN_ETIME,
	MDW_INFO_MAX_DTIME,

	MDW_INFO_RESERV_TIME_REMAIN,

	MDW_INFO_MAX,
};

enum mdw_info_dir {
	MDW_INFO_SET,
	MDW_INFO_GET
};

enum mdw_pwrplcy_type {
	MDW_POWERPOLICY_DEFAULT = 0, //do nothing
	MDW_POWERPOLICY_SUSTAINABLE = 1,
	MDW_POWERPOLICY_PERFORMANCE = 2,
	MDW_POWERPOLICY_POWERSAVING = 3,
};

enum {
	MDW_APPTYPE_DEFAULT = 0,
	MDW_APPTYPE_ONESHOT = 1,
	MDW_APPTYPE_STREAMING = 2,
};

struct mdw_device {
	enum mdw_driver_type driver_type;
	union {
		struct platform_device *pdev;
		struct rpmsg_device *rpdev;
	};
	struct device *dev;
	struct miscdevice *misc_dev;

	/* init flag */
	bool inited;

	/* cores enable bitmask */
	uint32_t dsp_mask;
	uint32_t dla_mask;
	uint32_t dma_mask;

	/* mdw version */
	uint32_t mdw_ver;

	/* device */
	struct apusys_device *adevs[MDW_DEV_MAX];

	/* device support information */
	unsigned long dev_mask[BITS_TO_LONGS(MDW_DEV_MAX)];
	struct mdw_dinfo *dinfos[MDW_DEV_MAX];
	/* memory support information */
	unsigned long mem_mask[BITS_TO_LONGS(MDW_MEM_TYPE_MAX)];
	struct mdw_mem minfos[MDW_MEM_TYPE_MAX];

	/* memory hlist */
	struct list_head m_list;
	struct mutex m_mtx;
	struct mutex mctl_mtx;

	/* cmd clear wq */
	struct mutex c_mtx;
	struct list_head d_cmds;
	struct work_struct c_wk;

	/* platform functions */
	const struct mdw_plat_func *plat_funcs;
	void *dev_specific;

	/* fence info */
	uint64_t base_fence_ctx;
	uint32_t num_fence_ctx;
	unsigned long fence_ctx_mask[BITS_TO_LONGS(MDW_FENCE_MAX_RINGS)];
	struct mutex f_mtx;

	/* cmd history */
	uint64_t idle_time_ts;
	uint64_t predict_cmd_ts[MDW_NUM_PREDICT_CMD];
	struct min_heap heap;
	atomic_t cmd_running;
	struct mutex h_mtx;

	/* power fast on/off */
	bool support_power_fast_on_off;
	enum mdw_power_type power_state;
	uint64_t max_dtime_ts;
	struct mutex dtime_mtx;
	struct mutex power_mtx;
	uint32_t power_gain_time_us;

	/* dtime setting function */
	void (*update_power_dtime)(int dtime);
};

struct mdw_fpriv {
	struct mdw_device *mdev;

	struct list_head mems;
	struct list_head invokes;
	struct mutex mtx;
	struct mdw_mem_pool cmd_buf_pool;
	struct idr cmds;
	struct list_head cmds_list;
	atomic_t active_cmds;
	atomic_t exec_seqno;

	uint64_t cb_head_device_va;

	/* ref count for cmd/mem */
	atomic_t active;
	struct kref ref;
	void (*get)(struct mdw_fpriv *mpriv);
	void (*put)(struct mdw_fpriv *mpriv);

	/* cmd history */
	struct list_head ch_list;
	uint32_t cmd_cnt;
	struct mutex ch_mtx;

	/* cmd execute id counter */
	uint32_t counter;
};

struct mdw_exec_info {
	struct mdw_cmd_exec_info c;
	struct mdw_subcmd_exec_info sc;
};

struct mdw_subcmd_kinfo {
	struct mdw_subcmd_info *info; //c->subcmds
	struct mdw_subcmd_cmdbuf *cmdbufs; //from usr
	struct mdw_mem **ori_cbs; //pointer to original cmdbuf
	struct mdw_subcmd_exec_info *sc_einfo;
	uint64_t *kvaddrs; //pointer to duplicated buf
	uint64_t *daddrs; //pointer to duplicated buf
	void *priv; //mdw_ap_sc
};

struct mdw_fence {
	struct dma_fence base_fence;
	struct mdw_device *mdev;
	spinlock_t lock;
	char name[32];
};

struct mdw_cmd_map_invoke {
	struct list_head c_node;
	struct mdw_mem_map *map;
};

struct mdw_cmd_history_tbl {
	/* history basic struct */
	uint64_t uid;
	struct list_head ch_tbl_node; //to mpriv
	uint64_t period_cnt;
	uint32_t num_subcmds;

	/* history cmd time info */
	uint64_t h_end_ts;
	uint64_t h_start_ts;
	uint64_t h_period;
	uint64_t h_exec_time;

	/* history subcmd einfo */
	struct mdw_subcmd_exec_info *h_sc_einfo;
};

struct mdw_cmd {
	pid_t pid;
	pid_t tgid;
	char comm[16];
	uint64_t kid;
	uint64_t uid;
	uint64_t rvid;
	uint32_t u_pid;
	uint32_t priority;
	uint32_t hardlimit;
	uint32_t softlimit;
	uint32_t power_save;
	uint32_t power_plcy;
	uint32_t power_dtime;
	uint32_t power_etime;
	uint32_t fastmem_ms;
	uint32_t app_type;
	uint32_t num_subcmds;
	uint32_t num_links;
	struct mdw_subcmd_info *subcmds; //from usr
	struct mdw_subcmd_kinfo *ksubcmds;
	uint32_t num_cmdbufs;
	uint32_t size_cmdbufs;
	uint32_t size_apummutable;
	struct mdw_mem *cmdbufs;
	struct mdw_mem *exec_infos;
	struct mdw_exec_info *einfos;
	uint8_t *adj_matrix;
	struct mdw_subcmd_link_v1 *links;

	struct mutex mtx;
	struct list_head u_item;
	struct list_head map_invokes; //mdw_cmd_map_invoke
	struct list_head d_node; //mdev->d_cmds

	int id;
	struct kref ref;
	atomic_t is_running;

	uint64_t start_ts;
	uint64_t end_ts;

	struct mdw_fpriv *mpriv;
	void *internal_cmd;
	int (*complete)(struct mdw_cmd *c, int ret);
	int (*del_internal)(struct mdw_cmd *c);

	struct mdw_fence *fence;
	struct work_struct t_wk;
	struct dma_fence *wait_fence;

	void *tbl_kva;
	/* history params */
	uint32_t inference_ms;
	uint32_t tolerance_ms;
	/* set dtime */
	uint64_t is_dtime_set;
	/* polling cmd result */

	/* ext operation */
	uint64_t ext_id; // for apuext unique id

	/* cmd poll */
	uint32_t cmd_state;
	struct completion cmplt;

	/* cmd exec id */
	uint64_t inference_id;

	/* cmd tag time */
	uint64_t enter_complt_time;
	uint64_t pb_put_time;
	uint64_t cmdbuf_out_time;
	uint64_t handle_cmd_result_time;
	uint64_t load_aware_pwroff_time;
	uint64_t enter_mpriv_release_time;
	uint64_t mpriv_release_time;
	uint64_t enter_rv_cb_time;
	uint64_t rv_cb_time;

};

struct mdw_plat_func {
	int (*late_init)(struct mdw_device *mdev);
	void (*late_deinit)(struct mdw_device *mdev);
	int (*sw_init)(struct mdw_device *mdev);
	void (*sw_deinit)(struct mdw_device *mdev);
	int (*prepare_cmd)(struct mdw_fpriv *mpriv, void *kdata);
	int (*get_cmdbuf)(struct mdw_fpriv *mpriv, struct mdw_cmd *c);
	int (*run_cmd)(struct mdw_fpriv *mpriv, struct mdw_cmd *c);
	void (*release_cmd)(struct mdw_fpriv *mpriv);
	int (*set_power)(struct mdw_device *mdev, uint32_t type, uint32_t idx, uint32_t boost);
	int (*ucmd)(struct mdw_device *mdev, uint32_t type, void *vaddr, uint32_t size);
	int (*set_param)(struct mdw_device *mdev, enum mdw_info_type type, uint32_t val);
	uint32_t (*get_info)(struct mdw_device *mdev, enum mdw_info_type type);
	int (*register_device)(struct apusys_device *adev);
	int (*unregister_device)(struct apusys_device *adev);
	int (*power_onoff)(struct mdw_device *mdev, enum mdw_power_type power_onoff);
	bool (*poll_cmd)(struct mdw_cmd *c);
	void (*cp_execinfo)(struct mdw_cmd *c);
	/* power budget funcs */
	// int (*pb_get)(enum mdw_pwrplcy_type type, uint32_t deboundce_ms);
	// int (*pb_put)(enum mdw_pwrplcy_type type);
};

#if IS_ENABLED(CONFIG_MTK_AEE_FEATURE)
#include <aee.h>
#define _mdw_exception(key, reason, args...) \
	do { \
		char info[150];\
		mdw_drv_err(reason, args); \
		if (snprintf(info, 150, "apu_mdw:" reason, args) > 0) { \
			aee_kernel_exception(info, \
				"\nCRDISPATCH_KEY:%s\n", key); \
		} else { \
			mdw_drv_err("apu_mdw: %s snprintf fail(%d)\n", __func__, __LINE__); \
		} \
	} while (0)
#define mdw_exception(reason, args...) _mdw_exception("APUSYS_MIDDLEWARE", reason, ##args)
#define dma_exception(reason, args...) _mdw_exception("APUSYS_EDMA", reason, ##args)
#define aps_exception(reason, args...) _mdw_exception("APUSYS_APS", reason, ##args)
#else
#define mdw_exception(reason, args...)
#define dma_exception(reason, args...)
#define aps_exception(reason, args...)
#endif

long mdw_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);
int mdw_hs_ioctl(struct mdw_fpriv *mpriv, void *data);
int mdw_cmd_ioctl(struct mdw_fpriv *mpriv, void *kdata);
int mdw_mem_ioctl(struct mdw_fpriv *mpriv, void *data);
int mdw_util_ioctl(struct mdw_fpriv *mpriv, void *data);

void mdw_mem_all_print(struct mdw_fpriv *mpriv);

void mdw_mem_put(struct mdw_fpriv *mpriv, struct mdw_mem *m);
struct mdw_mem *mdw_mem_get(struct mdw_fpriv *mpriv, int handle);
struct mdw_mem *mdw_mem_alloc(struct mdw_fpriv *mpriv, enum mdw_mem_type type,
	uint64_t size, uint64_t align, uint64_t flags, bool need_handle);
void mdw_mem_free(struct mdw_fpriv *mpriv, struct mdw_mem *m);
long mdw_mem_set_name(struct mdw_mem *m, const char *buf);
int mdw_mem_map(struct mdw_fpriv *mpriv, struct mdw_mem *m);
int mdw_mem_unmap(struct mdw_fpriv *mpriv, struct mdw_mem *m);
int mdw_mem_flush(struct mdw_fpriv *mpriv, struct mdw_mem *m);
int mdw_mem_invalidate(struct mdw_fpriv *mpriv, struct mdw_mem *m);
int mdw_mem_init(struct mdw_device *mdev);
void mdw_mem_deinit(struct mdw_device *mdev);
void mdw_mem_mpriv_release(struct mdw_fpriv *mpriv);

int mdw_sysfs_init(struct mdw_device *mdev);
void mdw_sysfs_deinit(struct mdw_device *mdev);

int mdw_dbg_init(struct mdw_device *mdev, struct dentry *dbg_root);
void mdw_dbg_deinit(void);

int mdw_dev_init(struct device *dev, struct mdw_device *mdev);
void mdw_dev_deinit(struct mdw_device *mdev);
void mdw_dev_session_create(struct mdw_fpriv *mpriv);
void mdw_dev_session_delete(struct mdw_fpriv *mpriv);
int mdw_dev_validation(struct mdw_fpriv *mpriv, uint32_t dtype,
	struct mdw_cmd *cmd, struct apusys_cmdbuf *cbs, uint32_t num);

uint64_t mdw_fence_ctx_alloc(struct mdw_device *mdev);
void mdw_fence_ctx_free(struct mdw_device *mdev, uint64_t ctx);
const char *mdw_fence_get_driver_name(struct dma_fence *fence);
const char *mdw_fence_get_timeline_name(struct dma_fence *fence);
bool mdw_fence_enable_signaling(struct dma_fence *fence);
void mdw_fence_release(struct dma_fence *fence);
int mdw_fence_init(struct mdw_cmd *c, int fd);

#endif
