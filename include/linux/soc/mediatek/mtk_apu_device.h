/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef __MTK_APU_DEVICE_H__
#define __MTK_APU_DEVICE_H__

#include <linux/dma-buf.h>


/* device type */
enum {
	APUSYS_DEVICE_NONE,

	APUSYS_DEVICE_SAMPLE,
	APUSYS_DEVICE_MDLA,
	APUSYS_DEVICE_VPU,
	APUSYS_DEVICE_EDMA,
	APUSYS_DEVICE_EDMA_LITE,
	APUSYS_DEVICE_MVPU,
	APUSYS_DEVICE_UP,
	APUSYS_DEVICE_APS,
	APUSYS_DEVICE_LAST,
	APUSYS_DEVICE_RT = 32,
	APUSYS_DEVICE_SAMPLE_RT,
	APUSYS_DEVICE_MDLA_RT,
	APUSYS_DEVICE_VPU_RT,
	APUSYS_DEVICE_EDMA_LITE_RT,
	APUSYS_DEVICE_MVPU_RT,
	APUSYS_DEVICE_UP_RT,
	APUSYS_DEVICE_APS_RT,

	APUSYS_DEVICE_MAX = 64, //total support 64 different devices
};

/* device cmd type */
enum {
	APUSYS_CMD_POWERON,
	APUSYS_CMD_POWERDOWN,
	APUSYS_CMD_RESUME,
	APUSYS_CMD_SUSPEND,
	APUSYS_CMD_EXECUTE,
	APUSYS_CMD_PREEMPT,

	APUSYS_CMD_FIRMWARE,
	APUSYS_CMD_USER,
	APUSYS_CMD_VALIDATE,
	APUSYS_CMD_SESSION_CREATE,
	APUSYS_CMD_SESSION_DELETE,

	APUSYS_CMD_MAX,
};

/* device preempt type */
enum {
	// device don't support preemption
	APUSYS_PREEMPT_NONE,
	// midware should resend preempted command after preemption call
	APUSYS_PREEMPT_RESEND,
	// midware don't need to resend cmd, and wait preempted cmd completed
	APUSYS_PREEMPT_WAITCOMPLETED,

	APUSYS_PREEMPT_MAX,
};

struct apusys_cmdbuf {
	void *kva;
	uint32_t size;
};

struct apusys_cmd_valid_handle {
	void *session;
	void *cmd;
	uint32_t num_cmdbufs;
	struct apusys_cmdbuf *cmdbufs;
};

/* device definition */
#define APUSYS_DEVICE_META_SIZE (32)

struct apusys_device {
	uint32_t dev_type;
	int idx;
	int preempt_type;
	uint8_t preempt_level;
	char meta_data[APUSYS_DEVICE_META_SIZE];

	void *private; // for hw driver to record private structure

	int (*send_cmd)(int type, void *hnd, struct apusys_device *dev);
};

int apusys_register_device(struct apusys_device *dev);
int apusys_unregister_device(struct apusys_device *dev);
int apusys_mem_validate_by_cmd(void *session, void *cmd, uint64_t eva, uint32_t size);
void *apusys_mem_query_kva_by_sess(void *session, uint64_t eva);
int apusys_mem_get_by_iova(void *session, uint64_t iova);
uint64_t apusys_mem_query_kva(uint64_t iova);
uint64_t apusys_mem_query_iova(uint64_t kva);
int apusys_mem_flush_kva(void *kva, uint32_t size);
int apusys_mem_invalidate_kva(void *kva, uint32_t size);

#endif
