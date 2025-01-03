// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Google LLC
 * Author: Bartłomiej Grzesik <bgrzesik@google.com>
 */
#include <kvm/iommu.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

/*
 * This value is reported for every single device. It does not matter what
 * value we use here. Since this modules is supposed to be used with devices
 * without smmu, no code will use this value.
 */
#define CONSTANT_IOMMU_ID 42

#ifndef MODULE
static bool enabled;

static int __init early_enabled_cfg(char *arg)
{
	if (!arg)
		return -EINVAL;

	enabled = !strcmp(arg, "1");

	return 0;
}

early_param("kvm-arm.unsafe_iommu", early_enabled_cfg);
#else
/* If the module is not builtin and loaded then enable the stub by default */
static bool enabled = true;
#endif

static int init_driver(void)
{
	return 0;
}

static void remove_driver(void)
{
}

pkvm_handle_t get_iommu_id(struct device *dev)
{
	return CONSTANT_IOMMU_ID;
}

static struct kvm_iommu_driver driver_ops = {
	.init_driver = init_driver,
	.remove_driver = remove_driver,
	.get_iommu_id = get_iommu_id,
};

int kvm_nvhe_sym(kvm_stub_iommu_init_hyp_module)(
	const struct pkvm_module_ops *ops);

static int kvm_iommu_stub_register(void)
{
	int ret;

	if (!enabled)
		return 0;

	ret = kvm_iommu_register_driver(&driver_ops);
	if (!ret)
		pr_warn("KVM IOMMU Stubbed. Do not run any confidential workloads in pVMs");
	else
		pr_err("Failed to register iommu driver ret=%d", ret);

	/*
	 * If failed to stub the driver, still report success. It's possible
	 * that a real iommu driver has loaded.
	 */
	return 0;
}

module_init(kvm_iommu_stub_register);
MODULE_AUTHOR("Bartłomiej Grzesik <bgrzesik@google.com>");
MODULE_DESCRIPTION("KVM IOMMU stub driver");
MODULE_LICENSE("GPL v2");
