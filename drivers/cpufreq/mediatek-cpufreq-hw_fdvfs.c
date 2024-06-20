// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/cpufreq.h>
#include <linux/topology.h>
#include "mediatek-cpufreq-hw_fdvfs.h"

static int fdvfs_support = UNSUPPORTED;
static void __iomem *fdvfs_base, *fdvfs_reg, *fdvfs_cci_reg;

static bool ioremap_fdvfs_reg(struct platform_device *pdev)
{
	fdvfs_reg = devm_platform_ioremap_resource_byname(pdev, "fdvfs-reg");
	if (!fdvfs_reg) {
		dev_err(&pdev->dev,"%s failed to map resource %d\n", __func__, FDVFS_REG);
		goto no_res;
	}

	fdvfs_cci_reg = devm_platform_ioremap_resource_byname(pdev, "fdvfs-cci-reg");
	if (!fdvfs_cci_reg) {
		dev_err(&pdev->dev,"%s failed to map resource %d\n", __func__, FDVFS_CCI_REG);
		goto no_res;
	}

	dev_dbg(&pdev->dev,"%s: fdvfs reg mapping success.\n", __func__);
	return true;

no_res:
	return false;
}

int check_fdvfs_support(struct device *dev)
{
	struct device_node *np;
	struct platform_device *pdev;
	int ret = 0;

	if (fdvfs_support != UNSUPPORTED)
		return fdvfs_support == SUPPORTED ? SUPPORTED : UNKNOWN;

	np = of_find_compatible_node(NULL, NULL, "mediatek,fdvfs");
	if (!np) {
		dev_err(dev, "%s: failed to find node\n", __func__);
		ret = -EINVAL;
		goto exit;
	}

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		dev_err(dev, "%s: failed to find fdvfs device\n", __func__);
		ret = -ENODEV;
		goto exit;
	}

	fdvfs_base = devm_platform_ioremap_resource_byname(pdev, "fdvfs-support");
	if (!fdvfs_base) {
		dev_err(dev, "%s: can't get mem resource\n", __func__);
		ret = -ENOMEM;
		goto exit;
	}

	if (readl_relaxed(fdvfs_base) == FDVFS_MAGICNUM && ioremap_fdvfs_reg(pdev)) {
		fdvfs_support = SUPPORTED;
		dev_dbg(dev, "%s: fdvfs supports\n", __func__);
	} else {
		fdvfs_support = UNKNOWN;
		dev_dbg(dev, "%s: fdvfs not supports\n", __func__);
	}

	of_node_put(np);
	return fdvfs_support;

exit:
	fdvfs_support = UNKNOWN;
	of_node_put(np);

	return ret;
}

int cpufreq_fdvfs_switch(unsigned int target_f, struct cpufreq_policy *policy)
{
	unsigned int cpu;

	target_f = DIV_ROUND_UP(target_f, FDVFS_FREQU);
	for_each_cpu(cpu, policy->real_cpus) {
		writel_relaxed(target_f, fdvfs_reg + cpu*CPU_OFS);
	}

	return 0;
}

bool cpufreq_fdvfs_cci_switch(unsigned int target_f)
{

	if (fdvfs_support != SUPPORTED || fdvfs_cci_reg == NULL)
		return false;

	target_f = DIV_ROUND_UP(target_f, FDVFS_FREQU);
	writel_relaxed(target_f, fdvfs_cci_reg);

	return true;
}

MODULE_AUTHOR("Haohao Yang <Haohao.yang@mediatek.com>");
MODULE_DESCRIPTION("fdvfs apis");
MODULE_LICENSE("GPL");
