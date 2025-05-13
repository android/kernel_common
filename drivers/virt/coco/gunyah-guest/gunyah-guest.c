// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <linux/gunyah.h>
#include <linux/init.h>
#include <linux/virtio_balloon.h>

#include <asm/hypervisor.h>

#define ADDRSPACE_INFO_AREA_ROOTVM_ADDRSPACE_CAP ((uint16_t)0)
struct addrspace_info_area_rootvm_addrspace_cap {
	u64 addrspace_cap;
	u32 rights;
	u32 res0;
};

static u64 our_addrspace_capid;

#ifdef CONFIG_VIRTIO_BALLOON_HYP_OPS
static void gunyah_page_relinquish(struct page *page,  unsigned int nr)
{
	/* Release page to Host, so unlock and sanitize */
	u64 flags = BIT_ULL(GUNYAH_ADDRSPC_MODIFY_FLAG_UNLOCK_BIT) |
			BIT_ULL(GUNYAH_ADDRSPC_MODIFY_FLAG_SANITIZE_BIT);
	phys_addr_t phys, end;
	int ret = 0;

	phys = page_to_phys(page);
	end = phys + PAGE_SIZE * nr;

	while (phys < end) {
		ret = gunyah_hypercall_addrspc_modify_pages(our_addrspace_capid,
				phys, PAGE_SIZE, flags);
		if (ret)
			pr_err_ratelimited("Failed to relinquish page: %016llx %d\n", phys, ret);

		phys += PAGE_SIZE;
	}

}

static void gunyah_post_page_relinquish_tlb_inv(void)
{
	/* Release page to Host, so unlock and sanitize */
	int ret = 0;

	ret = gunyah_hypercall_addrspc_modify_pages(our_addrspace_capid, 0, 0, 0);
	if (ret)
		pr_err_ratelimited("Failed to flush tlb: %d\n", ret);
}

static struct virtio_balloon_hyp_ops gunyah_virtio_balloon_hyp_ops = {
	.page_relinquish = gunyah_page_relinquish,
	.post_page_relinquish_tlb_inv = gunyah_post_page_relinquish_tlb_inv
};

#endif

static int __init gunyah_guest_init(void)
{
	struct addrspace_info_area_rootvm_addrspace_cap *info;
	size_t size;

	info = gunyah_get_info(GUNYAH_INFO_OWNER_ROOTVM, ADDRSPACE_INFO_AREA_ROOTVM_ADDRSPACE_CAP,
											&size);
	if (IS_ERR(info))
		return PTR_ERR(info);

	if (size != sizeof(*info))
		return -EINVAL;

	our_addrspace_capid = info->addrspace_cap;

#ifdef CONFIG_VIRTIO_BALLOON_HYP_OPS
	virtio_balloon_hyp_ops = &gunyah_virtio_balloon_hyp_ops;
#endif
	return 0;
}
core_initcall_sync(gunyah_guest_init);
