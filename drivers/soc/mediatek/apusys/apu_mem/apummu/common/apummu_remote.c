// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/rpmsg.h>
#include <linux/delay.h>
#include <linux/time64.h>

#include "apummu_cmn.h"
#include "apummu_drv.h"
#include "apummu_remote.h"
#include "apummu_msg.h"

struct apummu_msg_item {
	struct apummu_msg msg;
	struct list_head list;
};

struct apummu_msg *g_ammu_reply;
struct apummu_msg_mgr *g_ammu_msg;

static struct apummu_msg g_apummu_msg_reply;
static struct apummu_msg_mgr g_msg_mgr;

bool apummu_is_remote(void)
{
	bool is_remote = false;

	mutex_lock(&g_ammu_msg->lock.mutex_mgr);
	if (g_ammu_msg->info.init)
		is_remote = true;

	mutex_unlock(&g_ammu_msg->lock.mutex_mgr);

	return is_remote;
}

void apummu_remote_init(void)
{
	mutex_init(&g_msg_mgr.lock.mutex_cmd);
	mutex_init(&g_msg_mgr.lock.mutex_ipi);
	mutex_init(&g_msg_mgr.lock.mutex_mgr);

	spin_lock_init(&g_msg_mgr.lock.lock_rx);
	init_waitqueue_head(&g_msg_mgr.lock.wait_rx);

	INIT_LIST_HEAD(&g_msg_mgr.list_rx);

	g_ammu_reply = &g_apummu_msg_reply;
	g_ammu_reply->sn = 0;
	g_msg_mgr.count = 0;
	g_ammu_msg = &g_msg_mgr;
	g_msg_mgr.info.init = true;
}

void apummu_remote_exit(void)
{
	g_msg_mgr.info.init = false;
	g_ammu_reply->sn = 0;
}

int apummu_remote_send_cmd_sync(void *drvinfo, void *request, void *reply, uint32_t timeout)
{
	struct apummu_dev_info *rdv = NULL;
	int ret = 0;
	unsigned long flags;
	struct apummu_msg_item *item;
	struct list_head *tmp = NULL, *pos = NULL;
	struct apummu_msg *rmesg, *snd_rmesg;
	struct timespec64 begin, end, delta;
	int retry = 0;
	bool find = false;
	uint32_t *ptr;
	uint32_t cnt = 100, i = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	rdv = (struct apummu_dev_info *)drvinfo;

	snd_rmesg = (struct apummu_msg *) request;
	mutex_lock(&g_ammu_msg->lock.mutex_cmd);
	mutex_lock(&g_ammu_msg->lock.mutex_ipi);

	snd_rmesg->sn = g_ammu_msg->send_sn;
	g_ammu_msg->send_sn++;

	ptr = (uint32_t *)request;
	AMMU_LOG_VERBO("Send [%x][%x][%x][%x][%x][%x][%x][%x]\n",
			ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7]);

	/* send & retry */
	for (i = 0; i < cnt; i++) {
		ret = rpmsg_send(rdv->rpdev->ept, request, sizeof(struct apummu_msg));
		/* send busy, retry */
		if (ret == -EBUSY || ret == -EAGAIN) {
			if (!(i % 10))
				AMMU_LOG_INFO("re-send ipi(%u/%u)\n", i, cnt);
			if (ret == -EAGAIN && i < 10)
				usleep_range(200, 500);
			else if (ret == -EAGAIN && i < 50)
				usleep_range(1000, 2000);
			else if (ret == -EAGAIN && i < 200)
				usleep_range(10000, 20000);
			continue;
		}
		break;
	}

	mutex_unlock(&g_ammu_msg->lock.mutex_ipi);

	if (ret)
		AMMU_LOG_ERR("Send apummu IPI Fail %d\n", ret);

	ktime_get_ts64(&begin);
	do {
		ret = wait_event_interruptible_timeout(
				g_ammu_msg->lock.wait_rx,
				g_ammu_msg->count,
				msecs_to_jiffies(APUMMU_REMOTE_TIMEOUT));
		if (!ret) {
			AMMU_LOG_ERR("wait ACK timeout!!\n");
			ret = -ETIME;
			goto out;
		} else if (ret != -ERESTARTSYS) {
			break;
		}

		ktime_get_ts64(&end);
		delta = timespec64_sub(end, begin);
		if (delta.tv_sec > (APUMMU_REMOTE_TIMEOUT/1000)) {
			AMMU_LOG_ERR("wait ACK timeout manually!!\n");
			ret = -ETIME;
			goto out;
		}

		retry += 1;
		if (retry % 100 == 0)
			AMMU_LOG_WRN("Wake up by signal!, still waiting for ACK %d\n", retry);
		msleep(20);
	} while (ret == -ERESTARTSYS);

	spin_lock_irqsave(&g_ammu_msg->lock.lock_rx, flags);

	list_for_each_safe(pos, tmp, &g_ammu_msg->list_rx) {
		item = list_entry(pos, struct apummu_msg_item, list);
		list_del(pos);
		g_ammu_msg->count--;
		rmesg = (struct apummu_msg *) &item->msg;

		memcpy(reply, rmesg, sizeof(struct apummu_msg));
		find = true;
		break;
	}

	spin_unlock_irqrestore(&g_ammu_msg->lock.lock_rx, flags);

	if (find)
		vfree(item);

	ret = 0;
out:
	mutex_unlock(&g_ammu_msg->lock.mutex_cmd);
	return ret;
}


int apummu_remote_rx_cb(void *drvinfo, void *data, int len)
{
	unsigned long flags;
	struct apummu_dev_info *rdv = NULL;
	struct apummu_msg_item *item;
	uint32_t *ptr;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	rdv = (struct apummu_dev_info *)drvinfo;

	if (len != sizeof(struct apummu_msg)) {
		AMMU_LOG_ERR("invalid len %d / %zu\n", len, sizeof(struct apummu_msg));
		return -EINVAL;
	}

	item = vzalloc(sizeof(*item));

	memcpy(&item->msg, data, len);

	ptr = (uint32_t *)data;
	AMMU_LOG_VERBO("Rcv [%x][%x][%x][%x][%x][%x][%x][%x]\n",
			ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7]);

	spin_lock_irqsave(&g_ammu_msg->lock.lock_rx, flags);
	list_add_tail(&item->list, &g_ammu_msg->list_rx);
	g_ammu_msg->count++;
	spin_unlock_irqrestore(&g_ammu_msg->lock.lock_rx, flags);

	wake_up_interruptible(&g_ammu_msg->lock.wait_rx);

	return 0;
}

int apummu_remote_sync_sn(void *drvinfo, uint32_t sn)
{
	mutex_lock(&g_ammu_msg->lock.mutex_mgr);

	g_ammu_msg->send_sn = sn;

	mutex_unlock(&g_ammu_msg->lock.mutex_mgr);

	return 0;
}
