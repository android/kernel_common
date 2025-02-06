// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/pkvm.h>

#include <kvm/device.h>

struct pkvm_device *registered_devices;
unsigned long registered_devices_nr;

int pkvm_init_devices(void)
{
	size_t dev_sz;
	int ret;

	registered_devices = kern_hyp_va(registered_devices);
	dev_sz = PAGE_ALIGN(size_mul(sizeof(struct pkvm_device),
				     registered_devices_nr));

	ret = __pkvm_host_donate_hyp(hyp_virt_to_phys(registered_devices) >> PAGE_SHIFT,
				     dev_sz >> PAGE_SHIFT);
	if (ret)
		registered_devices_nr = 0;
	return ret;
}
