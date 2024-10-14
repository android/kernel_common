// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2021-2023, Intel Corporation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ":t7xx:%s: " fmt, __func__
#define dev_fmt(fmt) "t7xx: " fmt

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "t7xx_pci.h"
#include "t7xx_pci_rescan.h"

static struct remove_rescan_context t7xx_rescan_ctx;

void t7xx_pci_dev_rescan(void)
{
	struct pci_bus *b = NULL;

	pci_lock_rescan_remove();
	while ((b = pci_find_next_bus(b)))
		pci_rescan_bus(b);
	pci_unlock_rescan_remove();
}

void t7xx_rescan_done(void)
{
	if (!atomic_read(&t7xx_rescan_ctx.rescan_done)) {
		atomic_set(&t7xx_rescan_ctx.rescan_done, 1);
		pr_debug("Rescan probe\n");
	} else {
		pr_debug("Init probe\n");
	}
}

static void t7xx_remove_rescan(struct work_struct *work)
{
	int num_retries = RESCAN_RETRIES;
	struct pci_dev *pdev;

	atomic_set(&t7xx_rescan_ctx.rescan_done, 0);
	pdev = t7xx_rescan_ctx.dev;

	if (pdev) {
		pci_stop_and_remove_bus_device_locked(pdev);
		pr_debug("start remove and rescan flow\n");
	}

	do {
		t7xx_pci_dev_rescan();

		if (atomic_read(&t7xx_rescan_ctx.rescan_done))
			break;

		msleep(DELAY_RESCAN_MTIME);
	} while (num_retries--);
}

void t7xx_rescan_queue_work(struct pci_dev *pdev)
{
	if (!atomic_read(&t7xx_rescan_ctx.rescan_done)) {
		dev_err(&pdev->dev, "Rescan failed\n");
		return;
	}

	t7xx_rescan_ctx.dev = pdev;
	queue_work(t7xx_rescan_ctx.pcie_rescan_wq, &t7xx_rescan_ctx.service_task);
}

int t7xx_rescan_init(void)
{
	atomic_set(&t7xx_rescan_ctx.rescan_done, 1);
	t7xx_rescan_ctx.dev = NULL;

	t7xx_rescan_ctx.pcie_rescan_wq = create_singlethread_workqueue(MTK_RESCAN_WQ);
	if (!t7xx_rescan_ctx.pcie_rescan_wq) {
		pr_err("Failed to create workqueue: %s\n", MTK_RESCAN_WQ);
		return -ENOMEM;
	}

	INIT_WORK(&t7xx_rescan_ctx.service_task, t7xx_remove_rescan);

	return 0;
}

void t7xx_rescan_deinit(void)
{
	t7xx_rescan_ctx.dev = NULL;
	atomic_set(&t7xx_rescan_ctx.rescan_done, 0);
	cancel_work_sync(&t7xx_rescan_ctx.service_task);
	destroy_workqueue(t7xx_rescan_ctx.pcie_rescan_wq);
}
