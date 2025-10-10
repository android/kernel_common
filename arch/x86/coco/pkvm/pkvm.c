// SPDX-License-Identifier: GPL-2.0

#undef pr_fmt
#define pr_fmt(fmt)     "pkvm: " fmt

#include <linux/kvm_para.h>
#include <linux/io.h>
#include <asm/coco.h>
#include <asm/pkvm_guest.h>
#include <asm/pgtable.h>
#include <asm/apic.h>

DEFINE_STATIC_KEY_FALSE(pkvm_guest_detected);

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

static int pkvm_wakeup_secondary_cpu(u32 apic_id, unsigned long start_ip)
{
	return kvm_hypercall2(PKVM_GHC_START_CPU, apic_id, start_ip);
}

__init void pkvm_guest_init_coco(void)
{
	cc_vendor = CC_VENDOR_PKVM;

	static_branch_enable(&pkvm_guest_detected);

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

	apic_update_callback(wakeup_secondary_cpu, pkvm_wakeup_secondary_cpu);
}
