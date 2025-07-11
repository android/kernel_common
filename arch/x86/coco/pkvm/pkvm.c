// SPDX-License-Identifier: GPL-2.0

#undef pr_fmt
#define pr_fmt(fmt)     "pkvm: " fmt

#include <linux/cpufeature.h>
#include <linux/kvm_para.h>
#include <linux/io.h>
#include <asm/coco.h>
#include <asm/vmx.h>
#include <asm/pkvm_guest.h>
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

static void pkvm_get_ve_info(struct ve_info *ve)
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

static unsigned long pkvm_virt_mmio(int size, bool write, unsigned long vaddr,
				    unsigned long *val)
{
	unsigned long paddr;
	pte_t *pte;
	int level;

	pte = lookup_address(vaddr, &level);
	if (!pte)
		return -EIO;

	paddr = (pte_pfn(*pte) << PAGE_SHIFT) | (vaddr & ~page_level_mask(level));

	return write ? mmio_write(size, paddr, *val) : mmio_read(size, paddr, val);
}

static unsigned char pkvm_mmio_readb(const volatile void __iomem *addr)
{
	unsigned long val;

	if (!pkvm_virt_mmio(1, false, (unsigned long)addr, &val))
		return 0xff;
	return val;
}

static unsigned short pkvm_mmio_readw(const volatile void __iomem *addr)
{
	unsigned long val;

	if (!pkvm_virt_mmio(2, false, (unsigned long)addr, &val))
		return 0xffff;
	return val;
}

static unsigned int pkvm_mmio_readl(const volatile void __iomem *addr)
{
	unsigned long val;

	if (!pkvm_virt_mmio(4, false, (unsigned long)addr, &val))
		return 0xffffffff;
	return val;
}

static u64 pkvm_mmio_readq(const volatile void __iomem *addr)
{
	unsigned long val;

	if (!pkvm_virt_mmio(8, false, (unsigned long)addr, &val))
		return 0xffffffffffffffff;
	return val;
}

static void pkvm_mmio_writeb(unsigned char v, volatile void __iomem *addr)
{
	unsigned long val = v;

	pkvm_virt_mmio(1, true, (unsigned long)addr, &val);
}

static void pkvm_mmio_writew(unsigned short v, volatile void __iomem *addr)
{
	unsigned long val = v;

	pkvm_virt_mmio(2, true, (unsigned long)addr, &val);
}

static void pkvm_mmio_writel(unsigned int v, volatile void __iomem *addr)
{
	unsigned long val = v;

	pkvm_virt_mmio(4, true, (unsigned long)addr, &val);
}

static void pkvm_mmio_writeq(u64 v, volatile void __iomem *addr)
{
	unsigned long val = v;

	pkvm_virt_mmio(8, true, (unsigned long)addr, &val);
}

__init void pkvm_guest_init_coco(void)
{
	cc_vendor = CC_VENDOR_PKVM;

	pkvm_guest_detected = true;

	ve_x86_ops.mmio_read = mmio_read;
	ve_x86_ops.mmio_write = mmio_write;
	ve_x86_ops.handle_virt_exception = pkvm_handle_virt_exception;
	ve_x86_ops.get_ve_info = pkvm_get_ve_info;

	pv_ops.mmio.raw_readb = pkvm_mmio_readb;
	pv_ops.mmio.raw_readw = pkvm_mmio_readw;
	pv_ops.mmio.raw_readl = pkvm_mmio_readl;
	pv_ops.mmio.raw_readb_relaxed = pkvm_mmio_readb;
	pv_ops.mmio.raw_readw_relaxed = pkvm_mmio_readw;
	pv_ops.mmio.raw_readl_relaxed = pkvm_mmio_readl;
	pv_ops.mmio.raw_writeb = pkvm_mmio_writeb;
	pv_ops.mmio.raw_writew = pkvm_mmio_writew;
	pv_ops.mmio.raw_writel = pkvm_mmio_writel;
	pv_ops.mmio.raw_writeb_relaxed = pkvm_mmio_writeb;
	pv_ops.mmio.raw_writew_relaxed = pkvm_mmio_writew;
	pv_ops.mmio.raw_writel_relaxed = pkvm_mmio_writel;
#ifdef CONFIG_X86_64
	pv_ops.mmio.raw_readq = pkvm_mmio_readq;
	pv_ops.mmio.raw_readq_relaxed = pkvm_mmio_readq;
	pv_ops.mmio.raw_writeq = pkvm_mmio_writeq;
	pv_ops.mmio.raw_writeq_relaxed = pkvm_mmio_writeq;
#endif
	pv_ops.mmio.pci_mmcfg_readb = pkvm_mmio_readb;
	pv_ops.mmio.pci_mmcfg_readw = pkvm_mmio_readw;
	pv_ops.mmio.pci_mmcfg_readl = pkvm_mmio_readl;
	pv_ops.mmio.pci_mmcfg_writeb = pkvm_mmio_writeb;
	pv_ops.mmio.pci_mmcfg_writew = pkvm_mmio_writew;
	pv_ops.mmio.pci_mmcfg_writel = pkvm_mmio_writel;
}
