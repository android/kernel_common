// SPDX-License-Identifier: GPL-2.0
/*
 * UCSI driver for ChromeOS EC
 *
 * Copyright 2024 Google LLC.
 */

#include <linux/container_of.h>
#include <linux/dev_printk.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_usbpd_notify.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "ucsi.h"

/* MAX_EC_DATA_SIZE is the number of bytes that can be read from or written to
 * in the UCSI data structure using a single host command to the EC.
 */
#define MAX_EC_DATA_SIZE	256

/* WRITE_TMO_MS is the time within which a cmd complete or ack notification must
 * arrive after a command is sent to the PPM.
 */
#define WRITE_TMO_MS	5000

/* Number of times to attempt recovery from a write timeout before giving up. */
#define WRITE_TMO_CTR_MAX	5

struct cros_ucsi_data {
	struct device *dev;
	struct ucsi *ucsi;

	struct cros_ec_device *ec;
	struct notifier_block nb;
	struct work_struct work;
	struct delayed_work write_tmo;
	int tmo_counter;

	struct completion complete;
	unsigned long flags;
};

static int cros_ucsi_read(struct ucsi *ucsi, unsigned int offset, void *val,
			  size_t val_len)
{
	struct cros_ucsi_data *udata = ucsi_get_drvdata(ucsi);
	struct ec_params_ucsi_ppm_get req = {
		.offset = offset,
		.size = val_len,
	};
	int ret;

	if (val_len > MAX_EC_DATA_SIZE) {
		dev_err(udata->dev, "Can't read %zu bytes. Too big.", val_len);
		return -EINVAL;
	}

	ret = cros_ec_cmd(udata->ec, 0, EC_CMD_UCSI_PPM_GET,
			  &req, sizeof(req), val, val_len);
	if (ret < 0) {
		dev_warn(udata->dev, "Failed to send EC message UCSI_PPM_GET: error=%d", ret);
		return ret;
	}
	return 0;
}

static int cros_ucsi_async_write(struct ucsi *ucsi, unsigned int offset,
				 const void *val, size_t val_len)
{
	struct cros_ucsi_data *udata = ucsi_get_drvdata(ucsi);
	struct ec_params_ucsi_ppm_set *req;
	size_t req_len;
	int ret;

	if (val_len > MAX_EC_DATA_SIZE) {
		dev_err(udata->dev, "Can't write %zu bytes. Too big.", val_len);
		return -EINVAL;
	}

	req_len = sizeof(struct ec_params_ucsi_ppm_set) + val_len;
	req = kzalloc(req_len, GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	req->offset = offset;
	memcpy(req->data, val, val_len);
	ret = cros_ec_cmd(udata->ec, 0, EC_CMD_UCSI_PPM_SET,
			  req, req_len, NULL, 0);
	kfree(req);

	if (ret < 0) {
		dev_warn(udata->dev, "Failed to send EC message UCSI_PPM_SET: error=%d", ret);
		return ret;
	}
	return 0;
}

static int cros_ucsi_sync_write(struct ucsi *ucsi, unsigned int offset,
				const void *val, size_t val_len)
{
	struct cros_ucsi_data *udata = ucsi_get_drvdata(ucsi);
	bool ack = UCSI_COMMAND(*(u64 *)val) == UCSI_ACK_CC_CI;
	int ret;

	if (ack)
		set_bit(ACK_PENDING, &udata->flags);
	else
		set_bit(COMMAND_PENDING, &udata->flags);

	ret = cros_ucsi_async_write(ucsi, offset, val, val_len);
	if (ret)
		goto err;

	if (!wait_for_completion_timeout(&udata->complete,
					 msecs_to_jiffies(WRITE_TMO_MS))) {
		ret = -ETIMEDOUT;
		goto err;
	}

	/* Successful write. Cancel any pending recovery work. */
	cancel_delayed_work_sync(&udata->write_tmo);

	return 0;
err:
	/* EC may return -EBUSY if CCI.busy is set. Convert this to a timeout.
	 */
	if (ret == -EBUSY)
		ret = -ETIMEDOUT;

	/* Schedule recovery attempt when we timeout or tried to send a command
	 * while still busy.
	 */
	if (ret == -ETIMEDOUT) {
		cancel_delayed_work_sync(&udata->write_tmo);
		schedule_delayed_work(&udata->write_tmo,
				      msecs_to_jiffies(WRITE_TMO_MS));
	}

	if (ack)
		clear_bit(ACK_PENDING, &udata->flags);
	else
		clear_bit(COMMAND_PENDING, &udata->flags);
	return ret;
}

struct ucsi_operations cros_ucsi_ops = {
	.read = cros_ucsi_read,
	.async_write = cros_ucsi_async_write,
	.sync_write = cros_ucsi_sync_write,
};

static void cros_ucsi_work(struct work_struct *work)
{
	struct cros_ucsi_data *udata = container_of(work, struct cros_ucsi_data, work);
	u32 cci;

	if (cros_ucsi_read(udata->ucsi, UCSI_CCI, &cci, sizeof(cci)))
		return;

	if (UCSI_CCI_CONNECTOR(cci))
		ucsi_connector_change(udata->ucsi, UCSI_CCI_CONNECTOR(cci));

	if (cci & UCSI_CCI_ACK_COMPLETE &&
		test_and_clear_bit(ACK_PENDING, &udata->flags))
		complete(&udata->complete);
	if (cci & UCSI_CCI_COMMAND_COMPLETE &&
		test_and_clear_bit(COMMAND_PENDING, &udata->flags))
		complete(&udata->complete);
}

static void cros_ucsi_write_timeout(struct work_struct *work)
{
	struct cros_ucsi_data *udata =
		container_of(work, struct cros_ucsi_data, write_tmo.work);
	u32 cci;
	u64 cmd;

	if (cros_ucsi_read(udata->ucsi, UCSI_CCI, &cci, sizeof(cci))) {
		dev_err(udata->dev,
			"Reading CCI failed; no write timeout recovery possible.");
		return;
	}

	if (cci & UCSI_CCI_BUSY) {
		udata->tmo_counter++;

		if (udata->tmo_counter <= WRITE_TMO_CTR_MAX)
			schedule_delayed_work(&udata->write_tmo,
					      msecs_to_jiffies(WRITE_TMO_MS));
		else
			dev_err(udata->dev,
				"PPM unresponsive - too many write timeouts.");

		return;
	}

	/* No longer busy means we can reset our timeout counter. */
	udata->tmo_counter = 0;

	/* Need to ack previous command which may have timed out. */
	if (cci & UCSI_CCI_COMMAND_COMPLETE) {
		cmd = UCSI_ACK_CC_CI | UCSI_ACK_COMMAND_COMPLETE;
		cros_ucsi_async_write(udata->ucsi, UCSI_CONTROL, &cmd,
				      sizeof(cmd));

		/* Check again after a few seconds that the system has
		 * recovered to make sure our async write above was successful.
		 */
		schedule_delayed_work(&udata->write_tmo,
				      msecs_to_jiffies(WRITE_TMO_MS));
		return;
	}

	/* We recovered from a previous timeout. Treat this as a recovery from
	 * suspend and call resume.
	 */
	ucsi_resume(udata->ucsi);
}

static int cros_ucsi_event(struct notifier_block *nb,
			   unsigned long host_event, void *_notify)
{
	struct cros_ucsi_data *udata = container_of(nb, struct cros_ucsi_data, nb);

	if (!(host_event & PD_EVENT_PPM))
		return NOTIFY_OK;

	dev_dbg(udata->dev, "UCSI notification received");
	flush_work(&udata->work);
	schedule_work(&udata->work);

	return NOTIFY_OK;
}

static void cros_ucsi_destroy(struct cros_ucsi_data *udata)
{
	cros_usbpd_unregister_notify(&udata->nb);
	cancel_delayed_work_sync(&udata->write_tmo);
	cancel_work_sync(&udata->work);
	ucsi_destroy(udata->ucsi);
}

static int cros_ucsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_data = dev_get_drvdata(dev->parent);
	struct cros_ucsi_data *udata;
	int ret;

	udata = devm_kzalloc(dev, sizeof(*udata), GFP_KERNEL);
	if (!udata)
		return -ENOMEM;

	udata->dev = dev;

	udata->ec = ec_data->ec_dev;
	if (!udata->ec) {
		dev_err(dev, "couldn't find parent EC device");
		return -ENODEV;
	}

	platform_set_drvdata(pdev, udata);

	INIT_WORK(&udata->work, cros_ucsi_work);
	INIT_DELAYED_WORK(&udata->write_tmo, cros_ucsi_write_timeout);
	init_completion(&udata->complete);

	udata->ucsi = ucsi_create(dev, &cros_ucsi_ops);
	if (IS_ERR(udata->ucsi)) {
		dev_err(dev, "failed to allocate UCSI instance");
		return PTR_ERR(udata->ucsi);
	}

	ucsi_set_drvdata(udata->ucsi, udata);

	udata->nb.notifier_call = cros_ucsi_event;
	ret = cros_usbpd_register_notify(&udata->nb);
	if (ret) {
		dev_err(dev, "failed to register notifier: error=%d", ret);
		ucsi_destroy(udata->ucsi);
		return ret;
	}

	ret = ucsi_register(udata->ucsi);
	if (ret) {
		dev_err(dev, "failed to register UCSI: error=%d", ret);
		cros_ucsi_destroy(udata);
		return ret;
	}

	return 0;
}

static int cros_ucsi_remove(struct platform_device *dev)
{
	struct cros_ucsi_data *udata = platform_get_drvdata(dev);

	ucsi_unregister(udata->ucsi);
	cros_ucsi_destroy(udata);
	return 0;
}

static int __maybe_unused cros_ucsi_suspend(struct device *dev)
{
	struct cros_ucsi_data *udata = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&udata->write_tmo);
	cancel_work_sync(&udata->work);

	return 0;
}

static void __maybe_unused cros_ucsi_complete(struct device *dev)
{
	struct cros_ucsi_data *udata = dev_get_drvdata(dev);
	ucsi_resume(udata->ucsi);
}

static const struct dev_pm_ops cros_ucsi_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend = cros_ucsi_suspend,
	.complete = cros_ucsi_complete,
#endif
};

static const struct platform_device_id cros_ucsi_id[] = {
	{ KBUILD_MODNAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(platform, cros_ucsi_id);

static struct platform_driver cros_ucsi_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.pm = &cros_ucsi_pm_ops,
	},
	.id_table = cros_ucsi_id,
	.probe = cros_ucsi_probe,
	.remove = cros_ucsi_remove,
};

module_platform_driver(cros_ucsi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UCSI driver for ChromeOS EC");
