// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#ifndef __KVM_DEVICE_H
#define __KVM_DEVICE_H

#include <asm/kvm_host.h>

/*
 * @base: physical address of MMIO resource.
 * @size: size of resource in bytes.
 */
struct pkvm_dev_resource {
	u64 base;
	u64 size;
};

/*
 * @id: hypervisor ID of the IOMMU as defined by the driver.
 * @endpoint: endpoint ID of the device.
 */
struct pkvm_dev_iommu {
	u64 id;
	u64 endpoint;
};

#define PKVM_DEVICE_MAX_RESOURCE	32
#define PKVM_DEVICE_MAX_IOMMU		32

struct pkvm_device {
	struct pkvm_dev_resource resources[PKVM_DEVICE_MAX_RESOURCE];
	struct pkvm_dev_iommu iommus[PKVM_DEVICE_MAX_IOMMU];
	u32 nr_resources;
	u32 nr_iommus;
	u32 group_id;
	void *ctxt; /* Current context of the device*/
	unsigned short refcount;
	int (*reset_handler)(void *cookie, bool host_to_guest);
	void *cookie; /* cookie from drivers. */
};

#endif /* #ifndef __KVM_DEVICE_H */
