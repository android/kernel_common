/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */


#ifndef __APUSYS_APUMMU_REMOTE_H__
#define __APUSYS_APUMMU_REMOTE_H__

#define APUMMU_REMOTE_TIMEOUT	(40000) // RV ipi_send timeout after 30 sec

struct apummu_remote_info {
	bool init;
};
struct apummu_remote_lock {
	struct mutex mutex_cmd;
	struct mutex mutex_ipi;
	struct mutex mutex_mgr;
	spinlock_t lock_rx;
	wait_queue_head_t wait_rx;
};

struct apummu_msg_mgr {
	struct apummu_remote_lock lock;
	struct apummu_remote_info info;
	struct list_head list_rx;
	uint32_t count;
	uint32_t send_sn;
};

bool apummu_is_remote(void);
void apummu_remote_init(void);
void apummu_remote_exit(void);
int apummu_remote_send_cmd_sync(void *drvinfo, void *request, void *reply, uint32_t timeout);
int apummu_remote_rx_cb(void *drvinfo, void *data, int len);
int apummu_remote_sync_sn(void *drvinfo, uint32_t sn);

#endif
