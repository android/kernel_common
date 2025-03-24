/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _LINUX_GUNYAH_QTVM_H
#define _LINUX_GUNYAH_QTVM_H

#include <linux/notifier.h>
#include <linux/gunyah.h>

enum gunyah_qtvm_state {
	GUNYAH_QTVM_BEFORE_POWERUP    = 1,
	GUNYAH_QTVM_POWERUP_FAIL      = 2,
	GUNYAH_QTVM_EARLY_POWEROFF    = 3,
	GUNYAH_QTVM_POWEROFF          = 4,
	GUNYAH_QTVM_EXITED            = 5,
	GUNYAH_QTVM_CRASH             = 6,
};

#if IS_ENABLED(CONFIG_GUNYAH_QCOM_TRUSTED_VM)
int gunyah_qtvm_register_notifier(struct notifier_block *nb);
int gunyah_qtvm_unregister_notifier(struct notifier_block *nb);
int gunyah_qtvm_register_resource_ticket(struct gunyah_vm_resource_ticket *t,
							u16 vmid);
void gunyah_qtvm_unregister_resource_ticket(struct gunyah_vm_resource_ticket *t,
							u16 vmid);
#else
static inline int gunyah_qtvm_register_notifier(struct notifier_block *nb)
{
	return 0;
}
static inline int gunyah_qtvm_unregister_notifier(struct notifier_block *nb)
{
	return 0;
}
static inline int gunyah_qtvm_register_resource_ticket(
		struct gunyah_vm_resource_ticket *t, u16 vmid)
{
	return 0;
}
static inline void gunyah_qtvm_unregister_resource_ticket(
		struct gunyah_vm_resource_ticket *t, u16 vmid)
{
}
#endif

#endif
