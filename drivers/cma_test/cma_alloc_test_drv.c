// SPDX-License-Identifier: GPL-2.0
/*
 * Platform device driver to test CMA allocations
 *
 * The expected structure of the device tree for the dummy TPU is:
 *
 * / {
 *
 *	......
 *
 *	memory@40000000 {
 *		reg = <0x00 0x40000000 0x00 0x40000000>;
 *		device_type = "memory";
 *	};
 *
 *	reserved-memory {
 *		#address-cells = <2>;
 *		#size-cells = <2>;
 *		ranges;
 *
 *		// CMA reservation are taken from "shared-dma-pool".
 *		cma_test_reserve: cma_test_reserve {
 *			compatible = "shared-dma-pool";
 *			reusable;
 *			size = <0x0 0x2000000>; // 32 MiB
 *			alignment = <0x0 0x00001000>;
 *			alloc-ranges = <0x0 0x9 0x80000000 0x80000000>,
 *				<0x0 0x9 0x00000000 0x80000000>;
 *		};
 *	};
 *
 *	// This node uses the CMA reservation.
 *	cma_test_node {
 *		compatible = "cma_test_dummy";
 *		memory-region = <&cma_test_reserve>;
 *		state = "active";
 *	};
 *
 * The CMA stats can be checked with:
 *
 *   # cat /proc/meminfo | grep Cma
 *   CmaTotal:         229376 kB
 *   CmaFree:          228992 kB
 *
 * Note: The CmaTotal is the sum of:
 *
 *   - CMA reservations defined in the DTS.
 -   - CMA size passed as a kernel parameter. For example cma=192MB
 */

#define pr_fmt(fmt) "%s: %s: " fmt, KBUILD_MODNAME, __func__

#include <linux/cma.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/types.h>

#define SZ_32MiB (32 * 1024 * 1024)

struct cma *cma_ptr;
struct page *pages;
int nr_pages = SZ_32MiB / PAGE_SIZE;
char *buffer;

static const struct platform_device_id cma_test_id_table[] = {
	{ "cma_test_dev_id", 23 },
	{}, // Needs to finish with a NULL entry.
};
MODULE_DEVICE_TABLE(platform, cma_test_id_table);

// Identifies the node in the device tree.
static const struct of_device_id cma_test_dt_ids[] = {
	{ .compatible = "cma_test_dummy" },
	{}, // Needs to finish with a NULL entry.
};
MODULE_DEVICE_TABLE(of, cma_test_dt_ids);

static void rmem_remove_callback(void *dev)
{
	of_reserved_mem_device_release((struct device *)dev);
}

static int cma_test_probe(struct platform_device *pdev)
{
	int ret;
	const struct of_device_id *of_id;

	pr_info("Probing device");

	buffer = NULL;
	of_id = of_match_device(cma_test_dt_ids, &pdev->dev);
	if (!of_id) {
		pr_info("The node was not found in DTB");
		return -ENODEV;
	}

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret || !pdev->dev.cma_area) {
		dev_err(&pdev->dev,
			"The CMA reserved area is not assigned (ret %d)\n",
			ret);
		return -EINVAL;
	}

	ret = devm_add_action(&pdev->dev, rmem_remove_callback, &pdev->dev);
	if (ret) {
		of_reserved_mem_device_release(&pdev->dev);
		return ret;
	}

	cma_ptr = pdev->dev.cma_area;
	pages = cma_alloc(cma_ptr, nr_pages, get_order(PAGE_SIZE), false);
	if (pages) {
		pr_info("CMA allocation of %d pages was successful", nr_pages);
		buffer = kmap_local_page(pages);
		for (int i = 0; i < nr_pages; i++) {
			// Write one byte of every page.
			buffer[i * PAGE_SIZE] = 'A';
		}

	}

	return 0;
}

static void cma_test_remove(struct platform_device *pdev)
{
	pr_info("Remove cma alloc test driver");
}

static void cma_test_shutdown(struct platform_device *pdev)
{
	pr_info("Shutdown cma alloc test driver");
	if (pages) {
		pr_info("Unmapping %d CMA pages", nr_pages);
		kunmap_local(buffer);
	}
}

static int cma_test_suspend(struct platform_device *pdev, pm_message_t state)
{
	pr_info("Suspend cma alloc test driver");

	return 0;
}

static int cma_test_resume(struct platform_device *pdev)
{
	pr_info("Resume cma alloc test driver");

	return 0;
}

static struct platform_driver cma_test_driver = {
	.driver = {
		.name = "cma_alloc_test",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(cma_test_dt_ids),
	},
	.probe = cma_test_probe,
	.remove = cma_test_remove,
	.suspend = cma_test_suspend,
	.shutdown = cma_test_shutdown,
	.resume = cma_test_resume,
	.id_table = cma_test_id_table,
};

module_platform_driver(cma_test_driver);

MODULE_AUTHOR("Juan Yescas");
MODULE_DESCRIPTION("Platform Driver to test CMA allocations");
MODULE_LICENSE("GPL v2");
