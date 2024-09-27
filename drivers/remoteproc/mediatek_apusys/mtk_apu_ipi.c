// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/dev_printk.h>
#include <linux/irq.h>
#include <linux/pm_runtime.h>
#include <linux/sched/clock.h>

#include <linux/remoteproc/mtk_apu.h>
#include "mtk_apu_ipi_config.h"
#include "mtk_apu_rproc.h"

#define MTK_APU_MBOX_SEND_CNT	(4)
#define MTK_APU_MBOX_TIMEOUT	(1000)

struct mtk_apu_mbox_hdr {
	unsigned int id;
	unsigned int len;
	unsigned int serial_no;
	unsigned int csum;
};
struct mtk_apu_mbox_send_hdr {
	u8 send_cnt;
	struct mtk_apu_mbox_hdr *hdr;
};

static int power_dtime;

static void mtk_apu_update_power_dtime(int dtime)
{
	power_dtime = dtime;
}

static void mtk_apu_timer_callback(struct timer_list *timer)
{
	struct mtk_apu *apu = container_of(timer, struct mtk_apu, power_off_timer);

	mtk_apu_power_on_off(apu->pdev, MTK_APU_IPI_MIDDLEWARE, 0, 1);
}

static void mtk_apu_power_dtime_handler(struct mtk_apu *apu, int dtime)
{
	uint64_t ts = sched_clock();
	uint64_t dtime_ts;
	unsigned long power_dtime = 0;

	dtime = (dtime > MAX_DTIME)? MAX_DTIME: dtime;
	dtime = (dtime < MIN_DTIME)? MIN_DTIME: dtime;

	dtime_ts = ts + dtime;
	if (apu->cur_dtime_ts < dtime_ts)
		apu->cur_dtime_ts = dtime_ts;
	else
		return;

	if (timer_pending(&apu->power_off_timer)) {
		apu->ipi_pwr_ref_cnt[MTK_APU_IPI_MIDDLEWARE]--;
		apu->local_pwr_ref_cnt--;
	}

	power_dtime = msecs_to_jiffies(apu->cur_dtime_ts - ts);

	mod_timer(&apu->power_off_timer, jiffies + power_dtime);
}

static uint32_t calculate_csum(void *data, uint32_t len)
{
	uint32_t csum = 0, res = 0, i;
	uint8_t *ptr;

	for (i = 0; i < (len / sizeof(csum)); i++)
		csum += *(((uint32_t *)data) + i);

	ptr = (uint8_t *)data + len / sizeof(csum) * sizeof(csum);
	for (i = 0; i < (len % sizeof(csum)); i++)
		res |= *(ptr + i) << i * 8;

	csum += res;

	return csum;
}

static inline bool bypass_check(u32 id)
{
	/* whitelist IPI used in power off flow */
	return id == MTK_APU_IPI_DEEP_IDLE;
}

static void ipi_usage_cnt_update(struct mtk_apu *apu, u32 id, int diff)
{
	struct mtk_apu_ipi_desc *ipi = &apu->ipi_desc[id];

	if (ipi_attrs[id].ack != IPI_WITH_ACK)
		return;

	spin_lock(&apu->usage_cnt_lock);
	ipi->usage_cnt += diff;
	spin_unlock(&apu->usage_cnt_lock);
}

static int mtk_apu_ipi_check(struct mtk_apu *apu, u32 id, void *data, u32 len)
{
	bool forbid_ipi_send;

	if (!apu || id <= MTK_APU_IPI_INIT || id >= MTK_APU_IPI_MAX ||
	    id == MTK_APU_IPI_NS_SERVICE || len > MTK_APU_SHARE_BUFFER_SIZE || !data)
		return -EINVAL;

	mutex_lock(&apu->forbid_ipi_lock);
	forbid_ipi_send = apu->forbid_ipi_send;
	mutex_unlock(&apu->forbid_ipi_lock);

	if (forbid_ipi_send)
		return -ESHUTDOWN;

	if (!apu->platdata->flags.fast_on_off && !pm_runtime_enabled(apu->dev)) {
		dev_err(apu->dev, "%s: rpm disabled, ipi=%d\n", __func__, id);
		return -EBUSY;
	}

	if (ipi_attrs[id].direction == IPI_HOST_INITIATE &&
	    apu->ipi_inbound_locked == IPI_LOCKED && !bypass_check(id)) {
		return -EAGAIN;
	}

	return 0;
}

int mtk_apu_ipi_send(struct mtk_apu *apu, u32 id, void *data, u32 len, u32 wait_ms)
{
	struct timespec64 ts, te;
	struct mtk_apu_mbox_send_hdr send_hdr;
	unsigned long timeout;
	int ret = 0;

	if (apu->platdata->flags.fast_on_off && ipi_attrs[id].direction == IPI_HOST_INITIATE)
		mtk_apu_power_on_off(apu->pdev, id, 1, 0);

	ktime_get_ts64(&ts);

	mutex_lock(&apu->send_lock);
	ret = mtk_apu_ipi_check(apu, id, data, len);

	if (ret) {
		mutex_unlock(&apu->send_lock);
		return ret;
	}

	/*
	 * copy message payload to share buffer, need to do cache flush if
	 * the buffer is cacheable. currently not
	 */
	memcpy(apu->send_buf, data, len);

	send_hdr.hdr = kzalloc(sizeof(*send_hdr.hdr), GFP_KERNEL);

	send_hdr.send_cnt = MTK_APU_MBOX_SEND_CNT;
	send_hdr.hdr->id = id;
	send_hdr.hdr->len = len;
	send_hdr.hdr->csum = calculate_csum(data, len);
	send_hdr.hdr->serial_no = apu->tx_serial_no++;

	ipi_usage_cnt_update(apu, id, 1);

	ret = mbox_send_message(apu->ch, &send_hdr);
	if (ret < 0) {
		dev_err(apu->dev, "%s: failed to send msg, ret=%d\n", __func__, ret);
		goto unlock_mutex;
	} else {
		ret = 0;
	}

	apu->ipi_id = id;
	apu->ipi_id_ack[id] = false;

	/* poll ack from remote processor if wait_ms specified */
	if (wait_ms) {
		timeout = jiffies + msecs_to_jiffies(wait_ms);
		ret = wait_event_timeout(apu->ack_wq, apu->ipi_id_ack[id], timeout);

		apu->ipi_id_ack[id] = false;

		if (WARN(!ret, "apu ipi %d ack timeout!", id)) {
			ret = -EIO;
			goto unlock_mutex;
		} else {
			ret = 0;
		}
	}

unlock_mutex:
	kfree(send_hdr.hdr);
	mutex_unlock(&apu->send_lock);
	ktime_get_ts64(&te);
	ts = timespec64_sub(te, ts);

	dev_dbg(apu->dev, "%s: ipi_id=%d, len=%d, csum=%x, serial_no=%d, elapse=%lld\n",
		__func__, id, len, send_hdr.hdr->csum, send_hdr.hdr->serial_no,
		timespec64_to_ns(&ts));

	return ret;
}

int mtk_apu_ipi_register(struct mtk_apu *apu, u32 id, ipi_top_handler_t top_handler,
			 ipi_handler_t handler, void *priv)
{
	if (!apu || id >= MTK_APU_IPI_MAX || WARN_ON(!top_handler && !handler)) {
		if (apu)
			dev_err(apu->dev, "%s failed. apu=%ps, id=%d, handler=%ps, priv=%p\n",
				__func__, apu, id, handler, priv);
		return -EINVAL;
	}

	dev_dbg(apu->dev, "%s: ipi=%d, handler=%p, priv=%p", __func__, id, handler, priv);

	mutex_lock(&apu->ipi_desc[id].lock);
	apu->ipi_desc[id].top_handler = top_handler;
	apu->ipi_desc[id].handler = handler;
	apu->ipi_desc[id].priv = priv;
	mutex_unlock(&apu->ipi_desc[id].lock);

	return 0;
}

void mtk_apu_ipi_unregister(struct mtk_apu *apu, u32 id)
{
	if (!apu || id >= MTK_APU_IPI_MAX) {
		if (apu != NULL)
			dev_err(apu->dev, "%s: invalid id=%d\n", __func__, id);
		return;
	}

	mutex_lock(&apu->ipi_desc[id].lock);
	apu->ipi_desc[id].top_handler = NULL;
	apu->ipi_desc[id].handler = NULL;
	apu->ipi_desc[id].priv = NULL;
	mutex_unlock(&apu->ipi_desc[id].lock);
}

static int mtk_apu_init_ipi_top_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_apu *apu = priv;

	strscpy(apu->run.fw_ver, data, MTK_APU_FW_VER_LEN);

	apu->run.signaled = 1;
	wake_up_interruptible(&apu->run.wq);

	return IRQ_HANDLED;
}

static void mtk_apu_init_ipi_bottom_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_apu *apu = priv;
	int ret;

	strscpy(apu->run.fw_ver, data, MTK_APU_FW_VER_LEN);

	apu->run.signaled = 1;
	wake_up_interruptible(&apu->run.wq);

	ret = mtk_apu_power_on_off(apu->pdev, MTK_APU_IPI_INIT, 0, 1);
	if (ret)
		dev_err(apu->dev, "%s: call mtk_apu_power_on_off fail(%d)\n", __func__, ret);
}

static void mtk_apu_ipi_bottom_handle(struct mbox_client *cl, void *mssg)
{
	struct mtk_apu *apu = container_of(cl, struct mtk_apu, cl);
	int id, len;

	struct device *dev = apu->dev;
	ipi_handler_t handler;
	bool power_off_directly;

	id = apu->ipi_task.id;
	len = apu->ipi_task.len;

	if (id < 0 || id >= MTK_APU_IPI_MAX) {
		dev_err(apu->dev, "IPI id=%d is out of range", id);
		return;
	}

	mutex_lock(&apu->ipi_desc[id].lock);

	handler = apu->ipi_desc[id].handler;
	if (!handler) {
		dev_err(dev, "IPI id=%d is not registered", id);
		mutex_unlock(&apu->ipi_desc[id].lock);
	}

	apu->current_ipi_handler_id = id;

	handler(apu->temp_buf, len, apu->ipi_desc[id].priv);

	ipi_usage_cnt_update(apu, id, -1);

	apu->current_ipi_handler_id = -1;

	mutex_unlock(&apu->ipi_desc[id].lock);

	apu->ipi_id_ack[id] = true;

	wake_up(&apu->ack_wq);

	if (apu->platdata->flags.fast_on_off && ipi_attrs[id].direction == IPI_HOST_INITIATE) {
		if (power_dtime == 0) {
			power_off_directly = true;
		} else {
			mutex_lock(&apu->forbid_ipi_lock);
			power_off_directly = apu->forbid_ipi_send;
			mutex_unlock(&apu->forbid_ipi_lock);
		}

		if (power_off_directly) {
			mtk_apu_power_on_off(apu->pdev, id, 0, 1);
		} else {
			mtk_apu_power_dtime_handler(apu, power_dtime);
			power_dtime = 0;
		}
	}
}

static void mtk_apu_ipi_handle_rx(struct mbox_client *cl, void *mssg)
{
	struct mtk_apu *apu = container_of(cl, struct mtk_apu, cl);
	struct mtk_share_obj *recv_obj = apu->recv_buf;
	struct mtk_apu_mbox_hdr *hdr = mssg;
	u32 calc_csum;
	struct device *dev = apu->dev;
	ipi_top_handler_t top_handler;
	int ret;

	if (hdr->id >= MTK_APU_IPI_MAX) {
		dev_err(dev, "no such IPI id = %d\n", hdr->id);
		apu->ipi_task.id = -1;
		return;
	}

	if (hdr->len > MTK_APU_SHARE_BUFFER_SIZE) {
		dev_err(dev, "IPI message too long(len %d, max %d)\n",
			hdr->len, MTK_APU_SHARE_BUFFER_SIZE);
		apu->ipi_task.id = -1;
		return;
	}

	if (hdr->serial_no != apu->rx_serial_no) {
		dev_warn(dev, "unmatched serial_no: curr=%u, recv=%u\n",
			 apu->rx_serial_no, hdr->serial_no);
		/* correct the serial no. */
		apu->rx_serial_no = hdr->serial_no;
	}
	apu->rx_serial_no++;

	memcpy(apu->temp_buf, &recv_obj->share_buf, hdr->len);

	calc_csum = calculate_csum(apu->temp_buf, hdr->len);
	if (calc_csum != hdr->csum) {
		dev_err(dev, "csum error: recv=0x%08x, calc=0x%08x, skip\n",
			 hdr->csum, calc_csum);
		apu->ipi_task.id = -1;
		return;
	}

	/* execute top handler if exist */
	top_handler = apu->ipi_desc[hdr->id].top_handler;
	if (top_handler) {
		ret = top_handler(apu->temp_buf, hdr->len, apu->ipi_desc[hdr->id].priv);
		if (ret == IRQ_HANDLED)
			return;
	}
	apu->ipi_task.id = hdr->id;
	apu->ipi_task.len = hdr->len;
}

static int mtk_apu_send_ipi(struct platform_device *pdev, u32 id, void *buf,
			    unsigned int len, unsigned int wait)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);

	return mtk_apu_ipi_send(apu, id, buf, len, wait);
}

static int mtk_apu_register_ipi(struct platform_device *pdev, u32 id,
				ipi_handler_t handler, void *priv)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);

	return mtk_apu_ipi_register(apu, id, NULL, handler, priv);
}

static void mtk_apu_unregister_ipi(struct platform_device *pdev, u32 id)
{
	struct mtk_apu *apu = platform_get_drvdata(pdev);

	mtk_apu_ipi_unregister(apu, id);
}

int mtk_apu_power_on_off(struct platform_device *pdev, u32 id, u32 on, u32 off)
{
	int ret;
	struct mtk_apu *apu = platform_get_drvdata(pdev);
	struct device *dev;
	const struct mtk_apu_hw_ops *hw_ops;
	struct mtk_apu_ipi_desc *ipi;

	if (!apu)
		return -EINVAL;

	dev = apu->dev;

	if (id >= MTK_APU_IPI_MAX) {
		dev_err(dev, "%s: invalid ipi id = %d", __func__, id);
		return -EINVAL;
	}

	hw_ops = &apu->platdata->ops;

	if (!apu->platdata->flags.fast_on_off || !hw_ops->power_on_off)
		return -EOPNOTSUPP;

	ipi = &apu->ipi_desc[id];
	spin_lock(&apu->usage_cnt_lock);
	if (off == 1 && ipi_attrs[id].direction == IPI_HOST_INITIATE &&
	    (ipi->usage_cnt != 0 && apu->ipi_pwr_ref_cnt[id] <= 1) &&
	    !(ipi->usage_cnt == 1 && apu->current_ipi_handler_id == id &&
	    apu->ipi_pwr_ref_cnt[id] == 1)) {
		dev_err(dev, "%s: power off fail, ipi(%d) usage cnt(%d) not zero!\n",
			 __func__, id, ipi->usage_cnt);
		spin_unlock(&apu->usage_cnt_lock);
		ret = -EINVAL;
		return ret;
	}
	spin_unlock(&apu->usage_cnt_lock);

	ret = hw_ops->power_on_off(apu, id, on, off);

	return ret;
}

static struct mtk_rpmsg_info mtk_apu_rpmsg_info = {
	.send_ipi = mtk_apu_send_ipi,
	.register_ipi = mtk_apu_register_ipi,
	.unregister_ipi = mtk_apu_unregister_ipi,
	.ns_ipi_id = MTK_APU_IPI_NS_SERVICE,
};

static void mtk_apu_add_rpmsg_subdev(struct mtk_apu *apu)
{
	apu->rpmsg_subdev = mtk_rpmsg_create_rproc_subdev(to_platform_device(apu->dev),
							  &mtk_apu_rpmsg_info);

	if (apu->rpmsg_subdev)
		rproc_add_subdev(apu->rproc, apu->rpmsg_subdev);
}

static void mtk_apu_remove_rpmsg_subdev(struct mtk_apu *apu)
{
	if (apu->rpmsg_subdev) {
		rproc_remove_subdev(apu->rproc, apu->rpmsg_subdev);
		mtk_rpmsg_destroy_rproc_subdev(apu->rpmsg_subdev);
		apu->rpmsg_subdev = NULL;
	}
}

static void mtk_apu_ipi_tx_done(struct mbox_client *cl, void *msg, int ret)
{
	struct mtk_apu *apu = container_of(cl, struct mtk_apu, cl);

	if (ret < 0)
		dev_err(apu->dev, "tx not complete: sent:0x%p, ret:%d\n", msg, ret);
	else
		dev_dbg(apu->dev, "tx completed. sent:0x%p, ret:%d\n", msg, ret);
}

void mtk_apu_ipi_config_remove(struct mtk_apu *apu)
{
	dma_free_coherent(apu->dev, MTK_APU_SHARE_BUF_SIZE, apu->recv_buf, apu->recv_buf_da);
}

int mtk_apu_ipi_config_init(struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	struct mtk_apu_ipi_config *ipi_config;
	void *ipi_buf = NULL;
	dma_addr_t ipi_buf_da = 0;

	ipi_config = get_mtk_apu_config_user_ptr(apu->conf_buf, eAPU_IPI_CONFIG);

	/* initialize shared buffer */
	ipi_buf = dma_alloc_coherent(dev, MTK_APU_SHARE_BUF_SIZE, &ipi_buf_da, GFP_KERNEL);
	if (!ipi_buf || !ipi_buf_da) {
		dev_err(dev, "failed to allocate ipi share memory\n");
		return -ENOMEM;
	}

	dev_dbg(dev, "%s: ipi_buf=%p, ipi_buf_da=%pad\n", __func__, ipi_buf, &ipi_buf_da);

	apu->recv_buf = ipi_buf;
	apu->recv_buf_da = ipi_buf_da;
	apu->send_buf = ipi_buf + sizeof(struct mtk_share_obj);
	apu->send_buf_da = ipi_buf_da + sizeof(struct mtk_share_obj);

	ipi_config->in_buf_da = apu->send_buf_da;
	ipi_config->out_buf_da = apu->recv_buf_da;

	return 0;
}

int mtk_apu_ipi_init(struct platform_device *pdev, struct mtk_apu *apu)
{
	struct device *dev = apu->dev;
	struct mbox_client *cl;
	int i, ret;

	apu->tx_serial_no = 0;
	apu->rx_serial_no = 0;

	mutex_init(&apu->send_lock);
	mutex_init(&apu->power_lock);
	mutex_init(&apu->forbid_ipi_lock);
	spin_lock_init(&apu->usage_cnt_lock);

	for (i = 0; i < MTK_APU_IPI_MAX; i++) {
		mutex_init(&apu->ipi_desc[i].lock);
		lockdep_set_class_and_name(&apu->ipi_desc[i].lock, &ipi_lock_key[i],
					   ipi_attrs[i].name);
		apu->ipi_pwr_ref_cnt[i] = 0;
	}

	apu->current_ipi_handler_id = -1;

	init_waitqueue_head(&apu->run.wq);
	init_waitqueue_head(&apu->ack_wq);

	/* APU initialization IPI register */
	if (!apu->platdata->flags.fast_on_off) {
		ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_INIT, mtk_apu_init_ipi_top_handler,
					   NULL, apu);
		if (ret) {
			dev_err(dev, "failed to register mtk_apu_init_ipi_top_handler for init, ret=%d\n",
				ret);
			return ret;
		}
	} else {
		ret = mtk_apu_ipi_register(apu, MTK_APU_IPI_INIT, NULL,
					   mtk_apu_init_ipi_bottom_handler, apu);
		if (ret) {
			dev_err(dev, "failed to register mtk_apu_init_ipi_bottom_handler for init, ret=%d\n",
				ret);
			return ret;
		}
	}

	/* add rpmsg subdev */
	mtk_apu_add_rpmsg_subdev(apu);

	cl = &apu->cl;
	cl->dev = dev;
	cl->tx_block = true;
	cl->tx_tout = MTK_APU_MBOX_TIMEOUT;
	cl->knows_txdone = false;
	cl->rx_callback = mtk_apu_ipi_handle_rx;
	cl->rx_callback_bh = mtk_apu_ipi_bottom_handle;
	cl->tx_done = mtk_apu_ipi_tx_done;

	apu->ch = mbox_request_channel(cl, 0);

	if (IS_ERR(apu->ch)) {
		ret = PTR_ERR(apu->ch);
		dev_err_probe(dev, ret, "Failed to request mbox chan: %d\n", ret);
		goto remove_rpmsg_subdev;
	}

	timer_setup(&apu->power_off_timer, mtk_apu_timer_callback, 0);
	mtk_apu_init_mdw_dtime_setting(mtk_apu_update_power_dtime);

	return 0;

remove_rpmsg_subdev:
	mtk_apu_remove_rpmsg_subdev(apu);
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_INIT);

	return ret;
}

void mtk_apu_ipi_remove(struct mtk_apu *apu)
{
	if (timer_pending(&apu->power_off_timer))
		del_timer(&apu->power_off_timer);

	if (!IS_ERR(apu->ch))
		mbox_free_channel(apu->ch);
	mtk_apu_remove_rpmsg_subdev(apu);
	mtk_apu_ipi_unregister(apu, MTK_APU_IPI_INIT);
}
