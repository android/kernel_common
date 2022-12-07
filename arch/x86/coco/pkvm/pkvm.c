// SPDX-License-Identifier: GPL-2.0

#undef pr_fmt
#define pr_fmt(fmt)     "pkvm: " fmt

#include <linux/cpufeature.h>
#include <linux/kvm_para.h>
#include <asm/coco.h>
#include <asm/vmx.h>
#include <asm/pkvm.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>
#include <asm/pgtable.h>
#include <asm/virt_exception.h>

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

void pkvm_get_ve_info(struct ve_info *ve)
{
	/* Reuse the tdx output for pkvm. */
	struct tdx_module_args out;

	__pkvm_module_call(PKVM_GHC_GET_VE_INFO, &out);

	/* Transfer the output parameters */
	ve->exit_reason = out.rcx;
	ve->exit_qual   = out.rdx;
	ve->gla         = out.r8;
	ve->gpa         = out.r9;
}

static bool mmio_write(int size, unsigned long addr, unsigned long val)
{
	kvm_hypercall3(PKVM_GHC_IOWRITE, addr, size, val);

	return true;
}

static bool mmio_read(int size, unsigned long addr, unsigned long *val)
{
	*val = kvm_hypercall2(PKVM_GHC_IOREAD, addr, size);

	return true;
}

static int virt_exception_kernel(struct pt_regs *regs, struct ve_info *ve)
{
	switch (ve->exit_reason) {
	case EXIT_REASON_EPT_VIOLATION:
		return ve_handle_mmio(regs, ve);
	default:
		pr_warn("Unexpected #VE: %lld\n", ve->exit_reason);
		return -EIO;
	}
}

static bool pkvm_handle_virt_exception(struct pt_regs *regs, struct ve_info *ve)
{
	int insn_len;

	insn_len = virt_exception_kernel(regs, ve);
	if (insn_len < 0)
		return false;

	/* After successful #VE handling, move the IP */
	regs->ip += insn_len;

	return true;
}

__init void pkvm_guest_init_coco(void)
{
	cc_set_vendor(CC_VENDOR_PKVM);

	pkvm_guest_detected = true;

	ve_x86_ops.mmio_read = mmio_read;
	ve_x86_ops.mmio_write = mmio_write;
	ve_x86_ops.handle_virt_exception = pkvm_handle_virt_exception;
	ve_x86_ops.get_ve_info = pkvm_get_ve_info;
}
