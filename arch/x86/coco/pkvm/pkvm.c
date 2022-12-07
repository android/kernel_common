// SPDX-License-Identifier: GPL-2.0
#include <asm/pkvm.h>
#include <linux/kvm_para.h>

static bool pkvm_guest_detected;

bool pkvm_is_protected_guest(void)
{
	return pkvm_guest_detected;
}

int pkvm_set_mem_host_visibility(unsigned long addr, int numpages, bool enc)
{
	unsigned long size = numpages * PAGE_SIZE;

	if (!enc) {
		/*
		 * When pkvm guest want to share a range of memory, these pages
		 * may have not been setup in the guest ept pagetables. So when
		 * the pkvm do the __pkvm_guest_share_host() thing, if no page
		 * found in guest ept, this function will failed, thus the share
		 * page function will failed.
		 * So before share these pages to host, first touch them, so
		 * they will have entry in the guest ept, to make sure the
		 * sharing will success.
		 *
		 * TODO: Another good way to mitigate this touch is to fake ept
		 * violation when the sharing function find that there is no
		 * page in the guest ept.
		 */
		memset((void *)addr, 0, size);
		kvm_hypercall2(PKVM_GHC_SHARE_MEM, __pa(addr), size);
	} else
		kvm_hypercall2(PKVM_GHC_UNSHARE_MEM, __pa(addr), size);

	return 0;
}

__init void pkvm_guest_init_coco(void)
{
	cc_set_vendor(CC_VENDOR_PKVM);

	pkvm_guest_detected = true;
}
