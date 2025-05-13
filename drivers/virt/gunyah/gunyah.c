// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah: " fmt

#include <linux/gunyah.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

struct gunyah_info_desc {
	__le16 id;
	__le16 owner;
	__le32 size;
	__le32 offset;
#define INFO_DESC_VALID		BIT(31)
	__le32 flags;
};

static void *info_area;

void *gunyah_get_info(u16 owner, u16 id, size_t *size)
{
	struct gunyah_info_desc *desc = info_area;
	__le16 le_owner = cpu_to_le16(owner);
	__le16 le_id = cpu_to_le16(id);

	if (!desc)
		return ERR_PTR(-ENOENT);

	for (desc = info_area; le32_to_cpu(desc->offset); desc++) {
		if (!(le32_to_cpu(desc->flags) & INFO_DESC_VALID))
			continue;
		mb();
		if (le_owner == desc->owner && le_id == desc->id) {
			if (size)
				*size = le32_to_cpu(desc->size);
			return info_area + le32_to_cpu(desc->offset);
		}
	}
	return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL_GPL(gunyah_get_info);

static int __init gunyah_init(void)
{
	struct gunyah_hypercall_hyp_identify_resp gunyah_api;
	unsigned long info_ipa, info_size;
	enum gunyah_error gh_error;

	if (!arch_is_gunyah_guest())
		return -ENODEV;

	gunyah_hypercall_hyp_identify(&gunyah_api);

	pr_info("Running under Gunyah hypervisor %llx/v%u\n",
		FIELD_GET(GUNYAH_API_INFO_VARIANT_MASK, gunyah_api.api_info),
		gunyah_api_version(&gunyah_api));

	/* Might move this out to individual drivers if there's ever an API version bump */
	if (gunyah_api_version(&gunyah_api) != GUNYAH_API_V1) {
		pr_info("Unsupported Gunyah version: %u\n",
			gunyah_api_version(&gunyah_api));
		return -ENODEV;
	}

	gh_error = gunyah_hypercall_addrspace_find_info_area(&info_ipa, &info_size);
	/* ignore errors for compatability with gh without info_area support */
	if (gh_error != GUNYAH_ERROR_OK)
		return 0;

	info_area = memremap(info_ipa, info_size, MEMREMAP_WB);
	if (!info_area) {
		pr_err("Failed to map addrspace info area\n");
		return -ENOMEM;
	}

	return 0;
}
core_initcall(gunyah_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah Driver");
