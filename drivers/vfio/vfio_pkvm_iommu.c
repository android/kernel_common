// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 *
 * pKVM provides mutual distrust between host kernel and protected VMs(pVM)
 * One solution to provide DMA isolation in this model, is to move the IOMMU
 * control to the hypervisor and para-virtualize the IOMMU interface for
 * the host and guest kernels. (none of them have direct access to IOMMU
 * programming interface).
 * In the case of device assignment, the host can't map memory for the
 * guest kernel in the IOMMU (as it is not trusted).
 * So, what the host kernel would attach a blocking domain, when VFIO
 * assigns the device to user space, so it can't issue any DMA, and
 * when the guest take control it can program the IOMMU through hypervisor.
 * This looks similar to noiommu but with one main difference is that
 * group->type is VFIO_IOMMU, which attaches the groups to a blocking domain.
 */

#include <linux/module.h>
#include <linux/vfio.h>
#include "vfio.h"

static void *pkvm_iommu_open(unsigned long arg)
{
	if (arg != VFIO_PKVM_IOMMU)
		return ERR_PTR(-EINVAL);

	return NULL;
}

static void pkvm_iommu_release(void *iommu_data)
{
}

static long pkvm_iommu_ioctl(void *iommu_data,
			     unsigned int cmd, unsigned long arg)
{
	if (cmd == VFIO_CHECK_EXTENSION)
		return arg == VFIO_PKVM_IOMMU;

	return -ENOTTY;
}

static int pkvm_iommu_attach_group(void *iommu_data,
				   struct iommu_group *iommu_group,
				   enum vfio_group_type type)
{
	/*
	 * VFIO already calls iommu_group_claim_dma_owner() which attaches
	 * the group to a blocking domain.
	 */

	return 0;
}

static void pkvm_iommu_detach_group(void *iommu_data,
				    struct iommu_group *iommu_group)
{
	/*
	 * VFIO calls iommu_group_release_dma_owner().
	 */
}

static void pkvm_iommu_register_device(void *iommu_data,
				       struct vfio_device *vdev)
{
	vdev->protected = true;
}

static void pkvm_iommu_unregister_device(void *iommu_data,
					 struct vfio_device *vdev)
{
}

static const struct vfio_iommu_driver_ops pkvm_iommu_ops = {
	.name			= "vfio-pkvm-iommu",
	.owner			= THIS_MODULE,
	.open			= pkvm_iommu_open,
	.release		= pkvm_iommu_release,
	.ioctl			= pkvm_iommu_ioctl,
	.attach_group		= pkvm_iommu_attach_group,
	.detach_group		= pkvm_iommu_detach_group,
	.register_device	= pkvm_iommu_register_device,
	.unregister_device	= pkvm_iommu_unregister_device,
};

static int __init pkvm_iommu_init(void)
{
	return vfio_register_iommu_driver(&pkvm_iommu_ops);
}

static void __exit pkvm_iommu_exit(void)
{
	vfio_unregister_iommu_driver(&pkvm_iommu_ops);
}

module_init(pkvm_iommu_init);
module_exit(pkvm_iommu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("smostafa@google.com");
MODULE_DESCRIPTION("VFIO IOMMU for pKVM pvIOMMU");
