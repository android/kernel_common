// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <linux/of_platform.h>
#include <linux/arm-smccc.h>
#include <linux/iommu.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

struct pviommu {
	struct iommu_device		iommu;
	u32				id;
};

struct pviommu_master {
	struct device			*dev;
	struct pviommu			*iommu;
	u32				ssid_bits;
};

static int pviommu_map_pages(struct iommu_domain *domain, unsigned long iova,
			     phys_addr_t paddr, size_t pgsize, size_t pgcount,
			     int prot, gfp_t gfp, size_t *mapped)
{
	return 0;
}

static size_t pviommu_unmap_pages(struct iommu_domain *domain, unsigned long iova,
				  size_t pgsize, size_t pgcount,
				  struct iommu_iotlb_gather *gather)
{
	return 0;
}

static phys_addr_t pviommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	return 0;
}

static void pviommu_domain_free(struct iommu_domain *domain)
{
}

static int pviommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	return -ENODEV;
}

static struct iommu_domain *pviommu_domain_alloc(unsigned int type)
{
	return ERR_PTR(-ENODEV);
}

static struct platform_driver pkvm_pviommu_driver;

static struct pviommu *pviommu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev = driver_find_device_by_fwnode(&pkvm_pviommu_driver.driver, fwnode);

	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_ops pviommu_ops;

static struct iommu_device *pviommu_probe_device(struct device *dev)
{
	struct pviommu_master *master;
	struct pviommu *pv = NULL;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!fwspec)
		return ERR_PTR(-ENODEV);

	pv = pviommu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!pv)
		return ERR_PTR(-ENODEV);

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);

	master->dev = dev;
	master->iommu = pv;
	device_property_read_u32(dev, "pasid-num-bits", &master->ssid_bits);
	dev_iommu_priv_set(dev, master);

	return &pv->iommu;
}

static void pviommu_release_device(struct device *dev)
{
}

static int pviommu_of_xlate(struct device *dev, const struct of_phandle_args *args)
{
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

static struct iommu_group *pviommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	else
		return generic_device_group(dev);
}

static struct iommu_ops pviommu_ops = {
	.device_group		= pviommu_device_group,
	.of_xlate		= pviommu_of_xlate,
	.probe_device		= pviommu_probe_device,
	.release_device		= pviommu_release_device,
	.domain_alloc		= pviommu_domain_alloc,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= pviommu_attach_dev,
		.map_pages	= pviommu_map_pages,
		.unmap_pages	= pviommu_unmap_pages,
		.iova_to_phys	= pviommu_iova_to_phys,
		.free		= pviommu_domain_free,
	}
};

static int pviommu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pviommu *pv = devm_kmalloc(dev, sizeof(*pv), GFP_KERNEL);
	struct device_node *np = pdev->dev.of_node;
	int ret;
	struct arm_smccc_res res;

	ret = of_property_read_u32_index(np, "id", 0, &pv->id);
	if (ret) {
		dev_err(dev, "Failed to read id from device tree node %d\n", ret);
		return ret;
	}

	arm_smccc_1_1_hvc(ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID, 0, 0, 0, &res);
	if (res.a0 < 0)
		return -ENODEV;

	pviommu_ops.pgsize_bitmap = res.a0;

	ret = iommu_device_sysfs_add(&pv->iommu, dev, NULL,
				     "pviommu.%pa", &pv->id);

	ret = iommu_device_register(&pv->iommu, &pviommu_ops, dev);
	if (ret) {
		dev_err(dev, "Couldn't register %d\n", ret);
		iommu_device_sysfs_remove(&pv->iommu);
	}

	platform_set_drvdata(pdev, pv);

	return ret;
}

static const struct of_device_id pviommu_of_match[] = {
	{ .compatible = "pkvm,pviommu", },
	{ },
};

static struct platform_driver pkvm_pviommu_driver = {
	.probe = pviommu_probe,
	.driver = {
		.name = "pkvm-pviommu",
		.of_match_table = pviommu_of_match,
	},
};

module_platform_driver(pkvm_pviommu_driver);

MODULE_DESCRIPTION("IOMMU API for pKVM paravirtualized IOMMU");
MODULE_AUTHOR("Mostafa Saleh <smostafa@google.com>");
MODULE_LICENSE("GPL");
