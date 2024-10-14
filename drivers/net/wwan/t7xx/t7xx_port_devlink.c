// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023, Intel Corporation.
 */

#include <linux/vmalloc.h>

#include "t7xx_hif_cldma.h"
#include "t7xx_pci_rescan.h"
#include "t7xx_port.h"
#include "t7xx_port_ap_msg.h"
#include "t7xx_port_devlink.h"
#include "t7xx_port_proxy.h"
#include "t7xx_state_monitor.h"
#include "t7xx_uevent.h"

static struct t7xx_devlink_region_info t7xx_devlink_region_infos[] = {
	[T7XX_MRDUMP_INDEX] = {"mr_dump", T7XX_MRDUMP_SIZE},
	[T7XX_LKDUMP_INDEX] = {"lk_dump", T7XX_LKDUMP_SIZE},
};

static int t7xx_devlink_port_read(struct t7xx_port *port, char *buf, size_t count)
{
	struct sk_buff *skb;
	int ret, read_len;

	spin_lock_irq(&port->rx_wq.lock);
	if (skb_queue_empty(&port->rx_skb_list)) {
		ret = wait_event_interruptible_locked_irq(port->rx_wq,
							  !skb_queue_empty(&port->rx_skb_list));
		if (ret == -ERESTARTSYS) {
			spin_unlock_irq(&port->rx_wq.lock);
			return -EINTR;
		}
	}
	skb = skb_dequeue(&port->rx_skb_list);
	spin_unlock_irq(&port->rx_wq.lock);

	read_len = min_t(size_t, count, skb->len);
	memcpy(buf, skb->data, read_len);

	if (read_len < skb->len) {
		skb_pull(skb, read_len);
		skb_queue_head(&port->rx_skb_list, skb);
	} else {
		consume_skb(skb);
	}

	return read_len;
}

static int t7xx_devlink_port_write(struct t7xx_port *port, const char *buf, size_t count)
{
	const struct t7xx_port_conf *port_conf = port->port_conf;
	size_t actual = count, len, offset = 0;
	struct sk_buff *skb;
	int ret, txq_mtu;

	txq_mtu = t7xx_get_port_mtu(port);
	if (txq_mtu < 0)
		return -EINVAL;

	while (actual) {
		len = min_t(size_t, actual, txq_mtu);
		skb = __dev_alloc_skb(len, GFP_KERNEL);
		if (!skb)
			return -ENOMEM;

		skb_put_data(skb, buf + offset, len);
		ret = t7xx_port_send_raw_skb(port, skb);
		if (ret) {
			dev_err(port->dev, "write error on %s, size: %zu, ret: %d\n",
				port_conf->name, len, ret);
			dev_kfree_skb(skb);
			return ret;
		}

		offset += len;
		actual -= len;
	}

	return count;
}

static int t7xx_devlink_fb_handle_response(struct t7xx_port *port, char *data)
{
	char status[T7XX_FB_RESPONSE_SIZE + 1];
	int ret = 0, index, read_bytes;

	for (index = 0; index < T7XX_FB_RESP_COUNT; index++) {
		read_bytes = t7xx_devlink_port_read(port, status, T7XX_FB_RESPONSE_SIZE);
		if (read_bytes < 0) {
			dev_err(port->dev, "status read interrupted\n");
			ret = -EIO;
			break;
		}

		status[read_bytes] = '\0';
		dev_dbg(port->dev, "raw response from device: %s\n", status);
		if (!strncmp(status, T7XX_FB_RESP_INFO, strlen(T7XX_FB_RESP_INFO))) {
			break;
		} else if (!strncmp(status, T7XX_FB_RESP_OKAY, strlen(T7XX_FB_RESP_OKAY))) {
			break;
		} else if (!strncmp(status, T7XX_FB_RESP_FAIL, strlen(T7XX_FB_RESP_FAIL))) {
			ret = -EPROTO;
			break;
		} else if (!strncmp(status, T7XX_FB_RESP_DATA, strlen(T7XX_FB_RESP_DATA))) {
			if (data)
				snprintf(data, T7XX_FB_RESPONSE_SIZE, "%s",
					 status + strlen(T7XX_FB_RESP_DATA));
			break;
		}
	}

	return ret;
}

static int t7xx_devlink_fb_raw_command(char *cmd, struct t7xx_port *port, char *data)
{
	int ret, cmd_size = strlen(cmd);

	if (cmd_size > T7XX_FB_COMMAND_SIZE) {
		dev_err(port->dev, "command length %d is long\n", cmd_size);
		return -EINVAL;
	}

	if (cmd_size != t7xx_devlink_port_write(port, cmd, cmd_size)) {
		dev_err(port->dev, "raw command = %s write failed\n", cmd);
		return -EIO;
	}

	dev_dbg(port->dev, "raw command = %s written to the device\n", cmd);
	ret = t7xx_devlink_fb_handle_response(port, data);
	if (ret)
		dev_err(port->dev, "raw command = %s response FAILURE:%d\n", cmd, ret);

	return ret;
}

static int t7xx_devlink_fb_download_command(struct t7xx_port *port, size_t size)
{
	char download_command[T7XX_FB_COMMAND_SIZE];

	snprintf(download_command, sizeof(download_command), "%s:%08zx",
		 T7XX_FB_CMD_DOWNLOAD, size);
	return t7xx_devlink_fb_raw_command(download_command, port, NULL);
}

static int t7xx_devlink_fb_download(struct t7xx_port *port, const u8 *buf, size_t size)
{
	int ret;

	if (!size)
		return -EINVAL;

	ret = t7xx_devlink_fb_download_command(port, size);
	if (ret)
		return ret;

	ret = t7xx_devlink_port_write(port, buf, size);
	if (ret < 0)
		return ret;

	return t7xx_devlink_fb_handle_response(port, NULL);
}

static int t7xx_devlink_fb_flash(struct t7xx_port *port, const char *cmd)
{
	char flash_command[T7XX_FB_COMMAND_SIZE];

	snprintf(flash_command, sizeof(flash_command), "%s:%s", T7XX_FB_CMD_FLASH, cmd);
	return t7xx_devlink_fb_raw_command(flash_command, port, NULL);
}

static int t7xx_devlink_get_part_ver_fb_mode(struct t7xx_port *port, const char *cmd, char *data)
{
	char req_command[T7XX_FB_COMMAND_SIZE];

	snprintf(req_command, sizeof(req_command), "%s:%s", T7XX_FB_CMD_GET_VER, cmd);
	return t7xx_devlink_fb_raw_command(req_command, port, data);
}

static int t7xx_devlink_get_part_ver_norm_mode(struct t7xx_port *port, const char *cmd, char *data)
{
	char req_command[T7XX_FB_COMMAND_SIZE];
	int len;

	len = snprintf(req_command, sizeof(req_command), "%s:%s", T7XX_FB_CMD_GET_VER, cmd);
	t7xx_port_ap_msg_tx(port, req_command, len);

	return t7xx_devlink_fb_handle_response(port, data);
}

static int t7xx_devlink_fb_flash_partition(struct t7xx_port *port, const char *partition,
					   const u8 *buf, size_t size)
{
	int ret;

	ret = t7xx_devlink_fb_download(port, buf, size);
	if (ret < 0)
		return ret;

	return t7xx_devlink_fb_flash(port, partition);
}

static int t7xx_devlink_fb_get_core(struct t7xx_port *port)
{
	char mrdump_complete_event[T7XX_FB_EVENT_SIZE];
	u32 mrd_mb = T7XX_MRDUMP_SIZE / (1024 * 1024);
	struct t7xx_devlink *dl = port->t7xx_dev->dl;
	char mcmd[T7XX_FB_MCMD_SIZE + 1];
	size_t offset_dlen = 0;
	int clen, dlen, ret;

	dl->regions[T7XX_MRDUMP_INDEX].buf = vmalloc(dl->regions[T7XX_MRDUMP_INDEX].info->size);
	if (!dl->regions[T7XX_MRDUMP_INDEX].buf)
		return -ENOMEM;

	set_bit(T7XX_MRDUMP_STATUS, &dl->status);
	ret = t7xx_devlink_fb_raw_command(T7XX_FB_CMD_OEM_MRDUMP, port, NULL);
	if (ret) {
		dev_err(port->dev, "%s command failed\n", T7XX_FB_CMD_OEM_MRDUMP);
		goto free_mem;
	}

	while (dl->regions[T7XX_MRDUMP_INDEX].info->size > offset_dlen) {
		clen = t7xx_devlink_port_read(port, mcmd, sizeof(mcmd) - 1);
		mcmd[clen] = '\0';
		if (clen == strlen(T7XX_FB_CMD_RTS) && (!strcmp(mcmd, T7XX_FB_CMD_RTS))) {
			memset(mcmd, 0, sizeof(mcmd));
			if (t7xx_devlink_port_write(port, T7XX_FB_CMD_CTS, strlen(T7XX_FB_CMD_CTS))
						    != strlen(T7XX_FB_CMD_CTS)) {
				dev_err(port->dev, "write for _CTS failed:%zu\n",
					strlen(T7XX_FB_CMD_CTS));
				goto free_mem;
			}

			dlen = t7xx_devlink_port_read(port, dl->regions[T7XX_MRDUMP_INDEX].buf +
						      offset_dlen, T7XX_FB_MDATA_SIZE);
			if (dlen <= 0) {
				dev_err(port->dev, "read data error(%d)\n", dlen);
				ret = -EPROTO;
				goto free_mem;
			}
			offset_dlen += dlen;

			if (t7xx_devlink_port_write(port, T7XX_FB_CMD_FIN, strlen(T7XX_FB_CMD_FIN))
						    != strlen(T7XX_FB_CMD_FIN)) {
				dev_err(port->dev, "_FIN failed, (Read %05zu:%05zu)\n",
					strlen(T7XX_FB_CMD_FIN), offset_dlen);
				ret = -EPROTO;
				goto free_mem;
			}
			continue;
		} else if ((clen == strlen(T7XX_FB_RESP_MRDUMP_DONE)) &&
			   (!strcmp(mcmd, T7XX_FB_RESP_MRDUMP_DONE))) {
			dev_dbg(port->dev, "%s! size:%zd\n", T7XX_FB_RESP_MRDUMP_DONE, offset_dlen);
			snprintf(mrdump_complete_event, sizeof(mrdump_complete_event),
				 "%s size=%zu", T7XX_UEVENT_MRDUMP_READY, offset_dlen);
			t7xx_uevent_send(dl->port->dev, mrdump_complete_event);
			clear_bit(T7XX_MRDUMP_STATUS, &dl->status);
			return 0;
		}
		dev_err(port->dev, "getcore protocol error (read len %05d, response %s)\n",
			clen, mcmd);
		t7xx_uevent_send(dl->port->dev, T7XX_UEVENT_MRD_DISCD);
		ret = -EPROTO;
		goto free_mem;
	}

	dev_err(port->dev, "mrdump exceeds %uMB size. Discarded!\n", mrd_mb);

free_mem:
	vfree(dl->regions[T7XX_MRDUMP_INDEX].buf);
	clear_bit(T7XX_MRDUMP_STATUS, &dl->status);
	return ret;
}

static int t7xx_devlink_fb_dump_log(struct t7xx_port *port)
{
	char lkdump_complete_event[T7XX_FB_EVENT_SIZE];
	struct t7xx_devlink *dl = port->t7xx_dev->dl;
	struct t7xx_devlink_region *lkdump_region;
	char rsp[T7XX_FB_RESPONSE_SIZE];
	int dlen, datasize = 0, ret;
	size_t offset = 0;

	if (dl->status != T7XX_DEVLINK_IDLE) {
		dev_err(&dl->t7xx_dev->pdev->dev, "Modem is busy!\n");
		return -EBUSY;
	}

	set_bit(T7XX_LKDUMP_STATUS, &dl->status);
	ret = t7xx_devlink_fb_raw_command(T7XX_FB_CMD_OEM_LKDUMP, port, rsp);
	if (ret) {
		dev_err(port->dev, "%s command returns failure\n", T7XX_FB_CMD_OEM_LKDUMP);
		clear_bit(T7XX_LKDUMP_STATUS, &dl->status);
		return ret;
	}

	ret = kstrtoint(rsp, 16, &datasize);
	if (ret) {
		dev_err(port->dev, "kstrtoint error!\n");
		clear_bit(T7XX_LKDUMP_STATUS, &dl->status);
		return ret;
	}

	lkdump_region = &dl->regions[T7XX_LKDUMP_INDEX];
	if (datasize > lkdump_region->info->size) {
		dev_err(port->dev, "lkdump size is more than %dKB. Discarded!\n",
			T7XX_LKDUMP_SIZE / 1024);
		t7xx_uevent_send(dl->port->dev, T7XX_UEVENT_LKD_DISCD);
		clear_bit(T7XX_LKDUMP_STATUS, &dl->status);
		return -EPROTO;
	}

	lkdump_region->buf = vmalloc(lkdump_region->info->size);
	if (!lkdump_region->buf) {
		clear_bit(T7XX_LKDUMP_STATUS, &dl->status);
		return -ENOMEM;
	}

	while (datasize > 0) {
		dlen = t7xx_devlink_port_read(port, lkdump_region->buf + offset, datasize);
		if (dlen <= 0) {
			dev_err(port->dev, "lkdump read error ret = %d\n", dlen);
			clear_bit(T7XX_LKDUMP_STATUS, &dl->status);
			return -EPROTO;
		}

		datasize -= dlen;
		offset += dlen;
	}

	dev_dbg(port->dev, "LKDUMP DONE! size:%zd\n", offset);
	snprintf(lkdump_complete_event, sizeof(lkdump_complete_event), "%s size=%zu",
		 T7XX_UEVENT_LKDUMP_READY, offset);
	t7xx_uevent_send(dl->port->dev, lkdump_complete_event);
	clear_bit(T7XX_LKDUMP_STATUS, &dl->status);
	return t7xx_devlink_fb_handle_response(port, NULL);
}

static int t7xx_devlink_flash_update(struct devlink *devlink,
				     struct devlink_flash_update_params *params,
				     struct netlink_ext_ack *extack)
{
	struct t7xx_devlink *dl = devlink_priv(devlink);
	const char *component = params->component;
	const struct firmware *fw = params->fw;
	char flash_event[T7XX_FB_EVENT_SIZE];
	struct t7xx_port *port;
	int ret;

	if (dl->mode != T7XX_FB_DL_MODE) {
		dev_err(&dl->t7xx_dev->pdev->dev, "Modem is not in fastboot download mode!\n");
		ret = -EPERM;
		goto err_out;
	}

	if (dl->status != T7XX_DEVLINK_IDLE) {
		dev_err(&dl->t7xx_dev->pdev->dev, "Modem is busy!\n");
		ret = -EBUSY;
		goto err_out;
	}

	if (!component || !fw->data) {
		ret = -EINVAL;
		goto err_out;
	}

	set_bit(T7XX_FLASH_STATUS, &dl->status);
	port = dl->port;
	dev_dbg(port->dev, "flash partition name:%s binary size:%zu\n", component, fw->size);
	ret = t7xx_devlink_fb_flash_partition(port, component, fw->data, fw->size);
	if (ret) {
		devlink_flash_update_status_notify(devlink, "flashing failure!",
						   params->component, 0, 0);
		snprintf(flash_event, sizeof(flash_event), "%s for [%s]",
			 T7XX_UEVENT_FLASHING_FAILURE, params->component);
	} else {
		devlink_flash_update_status_notify(devlink, "flashing success!",
						   params->component, 0, 0);
		snprintf(flash_event, sizeof(flash_event), "%s for [%s]",
			 T7XX_UEVENT_FLASHING_SUCCESS, params->component);
	}
	t7xx_uevent_send(dl->port->dev, flash_event);
	clear_bit(T7XX_FLASH_STATUS, &dl->status);

err_out:
	return ret;
}

enum t7xx_devlink_param_id {
	T7XX_DEVLINK_PARAM_ID_BASE = DEVLINK_PARAM_GENERIC_ID_MAX,
	T7XX_DEVLINK_PARAM_ID_FASTBOOT,
};

static const struct devlink_param t7xx_devlink_params[] = {
	DEVLINK_PARAM_DRIVER(T7XX_DEVLINK_PARAM_ID_FASTBOOT,
			     "fastboot", DEVLINK_PARAM_TYPE_BOOL,
			     BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			     NULL, NULL, NULL),
};

bool t7xx_devlink_param_get_fastboot(struct devlink *devlink)
{
	union devlink_param_value saved_value;

	devl_param_driverinit_value_get(devlink, T7XX_DEVLINK_PARAM_ID_FASTBOOT,
					   &saved_value);
	return saved_value.vbool;
}

static int t7xx_devlink_reload_down(struct devlink *devlink, bool netns_change,
				    enum devlink_reload_action action,
				    enum devlink_reload_limit limit,
				    struct netlink_ext_ack *extack)
{
	struct t7xx_devlink *dl = devlink_priv(devlink);

	switch (action) {
	case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
		return 0;
	case DEVLINK_RELOAD_ACTION_FW_ACTIVATE:
		if (!dl->mode)
			return -EPERM;
		return t7xx_devlink_fb_raw_command(T7XX_FB_CMD_REBOOT, dl->port, NULL);
	default:
		/* Unsupported action should not get to this function */
		return -EOPNOTSUPP;
	}
}

static int t7xx_devlink_reload_up(struct devlink *devlink,
				  enum devlink_reload_action action,
				  enum devlink_reload_limit limit,
				  u32 *actions_performed,
				  struct netlink_ext_ack *extack)
{
	struct t7xx_devlink *dl = devlink_priv(devlink);
	*actions_performed = BIT(action);
	switch (action) {
	case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
	case DEVLINK_RELOAD_ACTION_FW_ACTIVATE:
		t7xx_rescan_queue_work(dl->t7xx_dev->pdev);
		return 0;
	default:
		/* Unsupported action should not get to this function */
		return -EOPNOTSUPP;
	}
}

static int t7xx_devlink_info_get(struct devlink *devlink, struct devlink_info_req *req,
				 struct netlink_ext_ack *extack)
{
	struct t7xx_devlink *dl = devlink_priv(devlink);
	char *part_name, *ver, *part_no, *data;
	int ret, total_part, i, ver_len;
	struct t7xx_port *port;

	port = dl->port;
	port->port_conf->ops->enable_chl(port);

	if (dl->status != T7XX_DEVLINK_IDLE) {
		dev_err(&dl->t7xx_dev->pdev->dev, "Modem is busy!\n");
		return -EBUSY;
	}

	data = kzalloc(T7XX_FB_RESPONSE_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	set_bit(T7XX_GET_INFO, &dl->status);
	if (dl->mode == T7XX_FB_DL_MODE)
		ret = t7xx_devlink_get_part_ver_fb_mode(port, "", data);
	else
		ret = t7xx_devlink_get_part_ver_norm_mode(port, "", data);

	if (ret < 0)
		goto err_clear_bit;

	part_no = strsep(&data, ",");
	if (kstrtoint(part_no, 16, &total_part)) {
		dev_err(&dl->t7xx_dev->pdev->dev, "kstrtoint error!\n");
		ret = -EINVAL;
		goto err_clear_bit;
	}

	for (i = 0; i < total_part; i++) {
		part_name = strsep(&data, ",");
		ver = strsep(&data, ",");
		ver_len = strlen(ver);
		if (ver[ver_len - 2] == 0x5C && ver[ver_len - 1] == 0x6E)
			ver[ver_len - 4] = '\0';
		ret = devlink_info_version_running_put_ext(req, part_name, ver,
							   DEVLINK_INFO_VERSION_TYPE_COMPONENT);
	}

err_clear_bit:
	clear_bit(T7XX_GET_INFO, &dl->status);
	kfree(data);
	return ret;
}

struct devlink_info_req {
	struct sk_buff *msg;
	void (*version_cb)(const char *version_name,
			   enum devlink_info_version_type version_type,
			   void *version_cb_priv);
	void *version_cb_priv;
};

struct devlink_flash_component_lookup_ctx {
	const char *lookup_name;
	bool lookup_name_found;
};

static int t7xx_devlink_info_get_loopback(struct devlink *devlink, struct devlink_info_req *req,
					  struct netlink_ext_ack *extack)
{
	struct devlink_flash_component_lookup_ctx *lookup_ctx = req->version_cb_priv;
	int ret;

	if (!req)
		return t7xx_devlink_info_get(devlink, req, extack);

	ret = devlink_info_version_running_put_ext(req, lookup_ctx->lookup_name,
						   "1.0", DEVLINK_INFO_VERSION_TYPE_COMPONENT);

	return ret;
}

/* Call back function for devlink ops */
static const struct devlink_ops devlink_flash_ops = {
	.supported_flash_update_params = DEVLINK_SUPPORT_FLASH_UPDATE_OVERWRITE_MASK,
	.flash_update = t7xx_devlink_flash_update,
	.reload_actions = BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT) |
			  BIT(DEVLINK_RELOAD_ACTION_FW_ACTIVATE),
	.info_get = t7xx_devlink_info_get_loopback,
	.reload_down = t7xx_devlink_reload_down,
	.reload_up = t7xx_devlink_reload_up,
};

static int t7xx_devlink_region_snapshot(struct devlink *dl, const struct devlink_region_ops *ops,
					struct netlink_ext_ack *extack, u8 **data)
{
	struct t7xx_devlink *t7xx_dl = devlink_priv(dl);
	struct t7xx_devlink_region *region = ops->priv;
	struct t7xx_port *port = t7xx_dl->port;
	u8 *snapshot_mem;

	if (t7xx_dl->status != T7XX_DEVLINK_IDLE)
		return -EBUSY;

	if (!strncmp(ops->name, "mr_dump", strlen("mr_dump"))) {
		snapshot_mem = vmalloc(region->info->size);
		memcpy(snapshot_mem, region->buf, region->info->size);
		*data = snapshot_mem;
	} else if (!strncmp(ops->name, "lk_dump", strlen("lk_dump"))) {
		int ret;

		ret = t7xx_devlink_fb_dump_log(port);
		if (ret)
			return ret;

		*data = region->buf;
	}

	return 0;
}

/* To create regions for dump files */
static int t7xx_devlink_create_regions(struct t7xx_devlink *dl)
{
	int ret, i;

	BUILD_BUG_ON(ARRAY_SIZE(t7xx_devlink_region_infos) > ARRAY_SIZE(dl->regions));
	for (i = 0; i < ARRAY_SIZE(t7xx_devlink_region_infos); i++) {
		dl->regions[i].info = &t7xx_devlink_region_infos[i];
		dl->regions[i].ops.name = dl->regions[i].info->name;
		dl->regions[i].ops.snapshot = t7xx_devlink_region_snapshot;
		dl->regions[i].ops.destructor = vfree;
		dl->regions[i].dlreg = devlink_region_create(dl->ctx, &dl->regions[i].ops,
							     T7XX_MAX_SNAPSHOTS,
							     t7xx_devlink_region_infos[i].size);
		if (IS_ERR(dl->regions[i].dlreg)) {
			ret = PTR_ERR(dl->regions[i].dlreg);
			dev_err(dl->port->dev, "devlink region fail,err %d\n", ret);
			for ( ; i >= 0; i--)
				devlink_region_destroy(dl->regions[i].dlreg);

			return ret;
		}

		dl->regions[i].ops.priv = &dl->regions[i];
	}

	return 0;
}

int t7xx_devlink_register(struct t7xx_pci_dev *t7xx_dev)
{
	union devlink_param_value value;
	struct devlink *dl_ctx;

	dl_ctx = devlink_alloc(&devlink_flash_ops, sizeof(struct t7xx_devlink),
			       &t7xx_dev->pdev->dev);
	if (!dl_ctx)
		return -ENOMEM;

	t7xx_dev->dl = devlink_priv(dl_ctx);
	t7xx_dev->dl->ctx = dl_ctx;
	t7xx_dev->dl->t7xx_dev = t7xx_dev;
	devlink_params_register(dl_ctx, t7xx_devlink_params, ARRAY_SIZE(t7xx_devlink_params));
	value.vbool = false;
	devl_param_driverinit_value_set(dl_ctx, T7XX_DEVLINK_PARAM_ID_FASTBOOT, value);
	devlink_register(dl_ctx);

	return 0;
}

void t7xx_devlink_unregister(struct t7xx_pci_dev *t7xx_dev)
{
	struct devlink *dl_ctx = t7xx_dev->dl->ctx;

	devlink_unregister(dl_ctx);
	devlink_params_unregister(dl_ctx, t7xx_devlink_params, ARRAY_SIZE(t7xx_devlink_params));
	devlink_free(dl_ctx);
}

static void t7xx_devlink_work(struct work_struct *work)
{
	struct t7xx_devlink *dl;

	dl = container_of(work, struct t7xx_devlink, ws);
	t7xx_devlink_fb_get_core(dl->port);
}

/**
 * t7xx_devlink_init - Initialize devlink to t7xx driver
 * @port: Pointer to port structure
 *
 * Returns: 0 on success and error values on failure
 */
static int t7xx_devlink_init(struct t7xx_port *port)
{
	struct t7xx_devlink *dl = port->t7xx_dev->dl;
	struct workqueue_struct *dl_wq;
	int rc;

	dl_wq = create_workqueue("t7xx_devlink");
	if (!dl_wq) {
		dev_err(port->dev, "create_workqueue failed\n");
		return -ENODATA;
	}

	INIT_WORK(&dl->ws, t7xx_devlink_work);
	port->rx_length_th = T7XX_MAX_QUEUE_LENGTH;

	dl->mode = T7XX_NORMAL_MODE;
	dl->status = T7XX_DEVLINK_IDLE;
	dl->wq = dl_wq;
	dl->port = port;

	rc = t7xx_devlink_create_regions(dl);
	if (rc) {
		destroy_workqueue(dl->wq);
		dev_err(port->dev, "devlink region creation failed, rc %d\n", rc);
		return -ENOMEM;
	}

	return 0;
}

static void t7xx_devlink_uninit(struct t7xx_port *port)
{
	struct t7xx_devlink *dl = port->t7xx_dev->dl;
	int i;

	vfree(dl->regions[T7XX_MRDUMP_INDEX].buf);

	dl->mode = T7XX_NORMAL_MODE;
	destroy_workqueue(dl->wq);

	BUILD_BUG_ON(ARRAY_SIZE(t7xx_devlink_region_infos) > ARRAY_SIZE(dl->regions));
	for (i = 0; i < ARRAY_SIZE(t7xx_devlink_region_infos); ++i)
		devlink_region_destroy(dl->regions[i].dlreg);

	skb_queue_purge(&port->rx_skb_list);
}

static int t7xx_devlink_enable_chl(struct t7xx_port *port)
{
	struct t7xx_devlink *dl = port->t7xx_dev->dl;

	t7xx_port_enable_chl(port);
	if (dl->mode == T7XX_FB_DUMP_MODE)
		queue_work(dl->wq, &dl->ws);

	return 0;
}

struct port_ops devlink_port_ops = {
	.init = &t7xx_devlink_init,
	.recv_skb = &t7xx_port_enqueue_skb,
	.uninit = &t7xx_devlink_uninit,
	.enable_chl = &t7xx_devlink_enable_chl,
	.disable_chl = &t7xx_port_disable_chl,
};
