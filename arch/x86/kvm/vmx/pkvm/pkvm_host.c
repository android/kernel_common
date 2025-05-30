// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmar.h>
#include <../drivers/iommu/intel/iommu.h>
#include <linux/pci.h>
#include <asm/pci_x86.h>
#include <asm/trapnr.h>
#include <asm/e820/api.h>
#include <asm/kvm_pkvm.h>

#include <mmu.h>
#include <mmu/spte.h>
#include <pkvm.h>
#include <pkvm_debugfs.h>
#include "pkvm_constants.h"

MODULE_LICENSE("GPL");

bool __read_mostly enable_pkvm = false;

static bool cmdline_pvmfw_present;
static u64 cmdline_pvmfw_base;
static u64 cmdline_pvmfw_size;

static int __init early_pkvm_parse_cmdline(char *buf)
{
	return kstrtobool(buf, &enable_pkvm);
}
early_param("kvm-intel.pkvm", early_pkvm_parse_cmdline);

static int __init early_pvmfw_parse_cmdline(char *buf)
{
	u64 start, size;
	char *p;

	size = memparse(buf, &p);
	if (p == buf || *p != '@')
		return -EINVAL;

	buf = p + 1;
	start = memparse(buf, &p);
	if (p == buf)
		return -EINVAL;

	/* clflush_cache_range() takes size as int */
	if (size > UINT_MAX)
		return -EINVAL;

	cmdline_pvmfw_present = true;
	cmdline_pvmfw_base = start;
	cmdline_pvmfw_size = size;
	return 0;
}
early_param("pvmfw", early_pvmfw_parse_cmdline);

static struct pkvm_hyp *pkvm;

struct pkvm_deprivilege_param {
	struct pkvm_hyp *pkvm;
	int ret;
};
DEFINE_PER_CPU_READ_MOSTLY(bool, pkvm_enabled);

struct pkvm_tlb_range {
	u64 start_gfn;
	u64 pages;
};

#define is_aligned(POINTER, BYTE_COUNT) \
		(((uintptr_t)(const void *)(POINTER)) % (BYTE_COUNT) == 0)

/* only need GDT entries for KERNEL_CS & KERNEL_DS as pKVM only use these two */
static struct gdt_page pkvm_gdt_page = {
	.gdt = {
		[GDT_ENTRY_KERNEL_CS]		= GDT_ENTRY_INIT(0xa09b, 0, 0xfffff),
		[GDT_ENTRY_KERNEL_DS]		= GDT_ENTRY_INIT(0xc093, 0, 0xfffff),
	},
};

static __init int check_pci_device_count(void)
{
	struct pci_dev *pdev = NULL;
	int devs = 0, devs_with_pasid = 0;

	/*
	 * pkvm has reserved the memory for IOMMU during early boot, and that
	 * memory is estimated with PKVM_MAX_PDEV_NUM and PKVM_MAX_PASID_PDEV_NUM.
	 * The actual number larger than this may cause IOMMU fail to create
	 * translation tables.
	 */
	for_each_pci_dev(pdev) {
		if (pdev->pasid_cap)
			devs_with_pasid++;
		else
			devs++;
	}

	if (devs > PKVM_MAX_PDEV_NUM ||
		devs_with_pasid > PKVM_MAX_PASID_PDEV_NUM) {
		pr_err("pkvm: Too many pdevs detected, actual %d %d max %d %d\n",
			devs, devs_with_pasid, PKVM_MAX_PDEV_NUM,
			PKVM_MAX_PASID_PDEV_NUM);
		return -EINVAL;
	}

	return 0;
}

/*
 * Check for the coherency of paging structures accessed through pasid table
 * entries (in scalable mode) or context table entries (in legacy mode).
 */
static inline bool is_iommu_coherent(u64 ecap)
{
	return ecap_smts(ecap) ? !!ecap_smpwc(ecap) : !!ecap_coherent(ecap);
}

static __init int check_and_init_iommu(struct pkvm_hyp *pkvm)
{
	struct pkvm_iommu_info *info;
	struct dmar_drhd_unit *drhd;
	int pgsz_mask = 1 << PG_LEVEL_4K;
	int pgt_level = 0;
	void __iomem *addr;
	u64 reg_size;
	u64 cap, ecap;
	int index = 0, ret;

/* matches with IOMMU cap SAGAW bits */
#define PGT_4LEVEL	BIT(2)
#define PGT_5LEVEL	BIT(3)

	ret = check_pci_device_count();
	if (ret)
		return ret;
	/*
	 * Some cases may require IOMMU and EPT to use both supported page
	 * table level and page size:
	 *
	 * 1) If IOMMU is working in nested translation of scalable-mode,
	 * pKVM may reuse EPT as the 2nd-level page table.
	 *
	 * 2) If IOMMU is working in legacy mode and a device is working
	 * in IOMMU pass-through mode, pKVM may reuse EPT as the 2nd-level
	 * page table.
	 *
	 * For other cases, though not necessary to use both IOMMU and EPT
	 * supported page table level and page size, using the same size
	 * can simplify the implementation, as pKVM doesn't need to check
	 * IOMMU types of all devices before deciding whether it's necessary
	 * to use both IOMMU and EPT supported page table level and page size.
	 */
	if (pkvm->vmx_cap.ept & VMX_EPT_PAGE_WALK_4_BIT)
		pgt_level |= PGT_4LEVEL;

	if (pkvm->vmx_cap.ept & VMX_EPT_PAGE_WALK_5_BIT)
		pgt_level |= PGT_5LEVEL;

	if (pkvm->vmx_cap.ept & VMX_EPT_2MB_PAGE_BIT)
		pgsz_mask |= 1 << PG_LEVEL_2M;

	if ((pkvm->vmx_cap.ept & VMX_EPT_1GB_PAGE_BIT))
		pgsz_mask |= 1 << PG_LEVEL_1G;

	pkvm->iommu_coherent = true;
	for_each_drhd_unit(drhd) {
		int level = 0, mask = 1 << PG_LEVEL_4K;

		if (index >= PKVM_MAX_IOMMU_NUM) {
			pr_err("pkvm: too many IOMMU devices to be supported\n");
			return -ENOMEM;
		}

		if (!drhd->reg_base_addr) {
			pr_err("pkvm: dmar unit not valid\n");
			return -EINVAL;
		}

		/*
		 * pkvm requires host IOMMU driver to work in scalable mode with
		 * first-level translation or legacy mode.
		 */
		if ((readl(drhd->iommu->reg + DMAR_GSTS_REG) & DMA_GSTS_TES) &&
			(readq(drhd->iommu->reg + DMAR_RTADDR_REG) & BIT(11))) {
			pr_err("pkvm: drhd reg_base 0x%llx: scalable/legacy mode not enabled\n",
				drhd->reg_base_addr);
			return -EINVAL;
		}

		addr = ioremap(drhd->reg_base_addr, VTD_PAGE_SIZE);
		if (!addr) {
			pr_err("pkvm: failed to map drhd reg physical addr 0x%llx\n",
				drhd->reg_base_addr);
			return -EINVAL;
		}

		info = &pkvm->iommu_infos[index];
		cap = readq(addr + DMAR_CAP_REG);
		ecap = readq(addr + DMAR_ECAP_REG);
		iounmap(addr);

		/*
		 * If pkvm IOMMU works in scalable mode, it requires to use nested translation,
		 * unless the host will use this IOMMU in passthrough mode only.
		 */
		if (ecap_smts(ecap) && !ecap_nest(ecap)) {
			pr_warn("pkvm: drhd reg_base 0x%llx: nested translation not supported\n",
				drhd->reg_base_addr);
			pr_warn("pkvm: drhd reg_base 0x%llx: do not use this iommu in non-passthrough mode!\n",
				drhd->reg_base_addr);
		}

		/*
		 * Check for the coherency of the paging structure access.
		 */
		if (!is_iommu_coherent(ecap))
			pkvm->iommu_coherent = false;

		info->reg_phys = drhd->reg_base_addr;
		reg_size = max_t(u64, ecap_max_iotlb_offset(ecap),
				 cap_max_fault_reg_offset(cap));
		info->reg_size = max_t(u64, reg_size, VTD_PAGE_SIZE);

		if (cap_sagaw(cap) & PGT_4LEVEL)
			level |= PGT_4LEVEL;
		if (cap_sagaw(cap) & PGT_5LEVEL)
			level |= PGT_5LEVEL;

		if (cap_super_page_val(cap) & BIT(0))
			mask |= 1 << PG_LEVEL_2M;
		if (cap_super_page_val(cap) & BIT(1))
			mask |= 1 << PG_LEVEL_1G;

		/* Get the both supported page table level */
		pgt_level &= level;
		pgsz_mask &= mask;

		index++;
	}

	/*
	 * There may be no supported page table level for both IOMMU and EPT.
	 * But there will always be both supported page size, which is 4K.
	 */
	if (pgt_level == 0) {
		pr_err("pkvm: no common page table level for IOMMU and EPT\n");
		return -EINVAL;
	}

	/* By default to use 4level */
	pkvm->ept_iommu_pgt_level = pgt_level & PGT_4LEVEL ? 4 : 5;

	pkvm->ept_iommu_pgsz_mask = pgsz_mask;

	return 0;
}

u64 hyp_total_reserve_pages(void)
{
	u64 total;

	total = pkvm_data_struct_pages(PKVM_GLOBAL_PAGES,
				       PKVM_PERCPU_PAGES,
				       num_possible_cpus());
	total += pkvm_vmemmap_pages(PKVM_VMEMMAP_ENTRY_SIZE);
	total += pkvm_mmu_pgtable_pages();
	total += host_ept_pgtable_pages();
	total += pkvm_shadow_ept_pgtable_pages(PKVM_MAX_VM_NUM);
	total += pkvm_iommu_pages(PKVM_MAX_PASID, PKVM_PASIDDEV_NUM,
				  PKVM_PDEV_NUM, PKVM_IOMMU_NUM,
				  PKVM_IOMMU_QI_DESC_SIZE,
				  PKVM_IOMMU_QI_DESC_STATUS_SIZE,
				  num_possible_cpus());
	total += pkvm_host_shadow_iommu_pgtable_pages(PKVM_PDEV_NUM);

	return total;
}

static struct vmcs *pkvm_alloc_vmcs(struct vmcs_config *vmcs_config_ptr)
{
	struct vmcs *vmcs;
	int pages = ALIGN(vmx_basic_vmcs_size(vmcs_config_ptr->basic), PAGE_SIZE) >> PAGE_SHIFT;

	vmcs = pkvm_sym(pkvm_early_alloc_contig)(pages);
	if (!vmcs)
		return NULL;

	memset(vmcs, 0, vmx_basic_vmcs_size(vmcs_config_ptr->basic));
	vmcs->hdr.revision_id = vmx_basic_vmcs_revision_id(vmcs_config_ptr->basic); /* vmcs revision id */

	return vmcs;
}

static inline void vmxon_setup_revid(void *vmxon_region)
{
	u32 rev_id = 0;
	u32 msr_high_value = 0;

	rdmsr(MSR_IA32_VMX_BASIC, rev_id, msr_high_value);

	memcpy(vmxon_region, &rev_id, 4);
}

static inline void cr4_set_vmxe(void)
{
	unsigned long cr4_value;

	cr4_value = __read_cr4();
	__write_cr4(cr4_value | X86_CR4_VMXE);
}

static inline void cr4_clear_vmxe(void)
{
	unsigned long cr4_value;

	cr4_value = __read_cr4();
	__write_cr4(cr4_value & ~(X86_CR4_VMXE));
}

static __init int pkvm_cpu_vmxon(u64 vmxon_pointer)
{
	u64 msr;

	cr4_set_vmxe();
	asm goto("1: vmxon %[vmxon_pointer]\n\t"
			  _ASM_EXTABLE(1b, %l[fault])
			  : : [vmxon_pointer] "m"(vmxon_pointer)
			  : : fault);
	return 0;

fault:
	WARN_ONCE(1, "VMXON faulted, MSR_IA32_FEAT_CTL (0x3a) = 0x%llx\n",
		  rdmsrl_safe(MSR_IA32_FEAT_CTL, &msr) ? 0xdeadbeef : msr);
	cr4_clear_vmxe();
	return -EFAULT;
}

static __init int pkvm_cpu_vmxoff(void)
{
	asm goto("1: vmxoff\n\t"
			  _ASM_EXTABLE(1b, %l[fault])
			  ::: "cc", "memory" : fault);
	cr4_clear_vmxe();
	return 0;

fault:
	cr4_clear_vmxe();
	return -EFAULT;
}

static __init int pkvm_enable_vmx(struct pkvm_host_vcpu *hvcpu)
{
	u64 phys_addr;

	hvcpu->vmxarea = pkvm_sym(pkvm_early_alloc_page)();
	if (!hvcpu->vmxarea)
		return -ENOMEM;

	phys_addr = __pa(hvcpu->vmxarea);
	if (!is_aligned(phys_addr, PAGE_SIZE))
		return -ENOMEM;

	/*setup revision id in vmxon region*/
	vmxon_setup_revid(hvcpu->vmxarea);

	return pkvm_cpu_vmxon(phys_addr);
}

static inline u32 get_ar(u16 sel)
{
	u32 access_rights;

	if (sel == 0) {
		access_rights = 0x10000;
	} else {
		asm ("lar %%ax, %%rax\n"
				: "=a"(access_rights) : "a"(sel));
		access_rights = access_rights >> 8;
		access_rights = access_rights & 0xf0ff;
	}

	return access_rights;
}

#define init_guestsegment(seg, SEG, base, limit)		\
	do  {							\
		u16 sel;					\
		u32 ar;						\
								\
		savesegment(seg, sel);				\
		ar = get_ar(sel);				\
		vmcs_write16(GUEST_##SEG##_SELECTOR, sel);	\
		vmcs_write32(GUEST_##SEG##_AR_BYTES, ar);	\
		vmcs_writel(GUEST_##SEG##_BASE, base);		\
		vmcs_write32(GUEST_##SEG##_LIMIT, limit);	\
	} while (0)

static __init void init_guest_state_area_from_native(int cpu)
{
	u16 ldtr;
	struct desc_ptr dt;
	unsigned long msrl;
	u32 high, low;

	/* load CR regiesters */
	vmcs_writel(GUEST_CR0, read_cr0() & ~X86_CR0_TS);
	vmcs_writel(GUEST_CR3, __read_cr3());
	vmcs_writel(GUEST_CR4, native_read_cr4());

	/* load cs/ss/ds/es */
	init_guestsegment(cs, CS, 0x0, 0xffffffff);
	init_guestsegment(ss, SS, 0x0, 0xffffffff);
	init_guestsegment(ds, DS, 0x0, 0xffffffff);
	init_guestsegment(es, ES, 0x0, 0xffffffff);

	/* load fs/gs */
	rdmsrl(MSR_FS_BASE, msrl);
	init_guestsegment(fs, FS, msrl, 0xffffffff);
	rdmsrl(MSR_GS_BASE, msrl);
	init_guestsegment(gs, GS, msrl, 0xffffffff);

	/* load GDTR */
	native_store_gdt(&dt);
	vmcs_writel(GUEST_GDTR_BASE, dt.address);
	vmcs_write32(GUEST_GDTR_LIMIT, dt.size);

	/* load TR */
	vmcs_write16(GUEST_TR_SELECTOR, GDT_ENTRY_TSS*8);
	vmcs_write32(GUEST_TR_AR_BYTES, get_ar(GDT_ENTRY_TSS*8));
	vmcs_writel(GUEST_TR_BASE, (unsigned long)&get_cpu_entry_area(cpu)->tss.x86_tss);
	vmcs_write32(GUEST_TR_LIMIT, __KERNEL_TSS_LIMIT);

	/* load LDTR */
	store_ldt(ldtr);
	vmcs_write16(GUEST_LDTR_SELECTOR, ldtr);
	vmcs_write32(GUEST_LDTR_AR_BYTES, 0x10000);
	vmcs_writel(GUEST_LDTR_BASE, 0x0);
	vmcs_write32(GUEST_LDTR_LIMIT, 0xffffffff);

	store_idt(&dt);
	vmcs_writel(GUEST_IDTR_BASE, dt.address);
	vmcs_write32(GUEST_IDTR_LIMIT, dt.size);

	/* set MSRs */
	vmcs_write64(GUEST_IA32_DEBUGCTL, 0);

	rdmsr(MSR_IA32_SYSENTER_CS, low, high);
	vmcs_write32(GUEST_SYSENTER_CS, low);

	rdmsrl(MSR_IA32_SYSENTER_ESP, msrl);
	vmcs_writel(GUEST_SYSENTER_ESP, msrl);

	rdmsrl(MSR_IA32_SYSENTER_EIP, msrl);
	vmcs_writel(GUEST_SYSENTER_EIP, msrl);

	rdmsrl(MSR_EFER, msrl);
	vmcs_write64(GUEST_IA32_EFER, msrl);

	rdmsrl(MSR_IA32_CR_PAT, msrl);
	vmcs_write64(GUEST_IA32_PAT, msrl);
}

static __init void init_guest_state_area(struct pkvm_host_vcpu *hvcpu, int cpu)
{
	init_guest_state_area_from_native(cpu);

	/*Guest non register state*/
	vmcs_write32(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
	vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, 0);
	vmcs_writel(GUEST_PENDING_DBG_EXCEPTIONS, 0);
	vmcs_write64(VMCS_LINK_POINTER, -1ull);
}

/*
 * [pcpu->stack, pcpu->stack + PKVM_STACK_SIZE) is per cpu pvm stack.
 * It is used as stack when the pcpu enters pKVM, i.e. HOST stack from
 * VMX point of view.
 *
 * Within the top of stack, a small region starting from stack_resv
 * is reserved  to store private paremeters,
 *
 *
 * ------------ Stack layout ----------
 * stack_top:
 * stack_resv + 8:	struct kvm_vcpu *vcpu
 * stack_resv + 0:	pointer to vcpu->arch.regs
 * stack_resv: (stack_top - PKVM_STACK_TOP_RESV)
 * 		VMCS.HOST_RSP for host VCPU
 *              .........
 *              .........
 * hstack_bottom:
 *
 */
static __init void init_host_state_area(struct pkvm_host_vcpu *hvcpu, int cpu)
{
	struct pkvm_pcpu *pcpu = hvcpu->pcpu;
	unsigned long host_rsp = get_host_stack_top(pcpu) - PKVM_STACK_TOP_RESV;
	struct kvm_vcpu *vcpu = &hvcpu->vmx.vcpu;

	pkvm_sym(pkvm_init_host_state_area)(pcpu, cpu);

	*((struct kvm_vcpu **) (host_rsp + 8)) = vcpu;
	*((unsigned long **) host_rsp) = vcpu->arch.regs;

	vmcs_writel(HOST_RIP, (unsigned long)pkvm_sym(__pkvm_vmexit_entry));
}

static __init void init_execution_control(struct pkvm_host_vcpu *hvcpu,
			    struct vmcs_config *vmcs_config_ptr,
			    struct vmx_capability *vmx_cap)
{
	struct vcpu_vmx *vmx = &hvcpu->vmx;
	u32 cpu_based_exec_ctrl = vmcs_config_ptr->cpu_based_exec_ctrl;
	u32 cpu_based_2nd_exec_ctrl = vmcs_config_ptr->cpu_based_2nd_exec_ctrl;

	pin_controls_set(vmx, vmcs_config_ptr->pin_based_exec_ctrl);

	/*
	 * CR3 LOAD/STORE EXITING are not used by pkvm
	 * INTR/NMI WINDOW EXITING are toggled dynamically
	 */
	cpu_based_exec_ctrl &= ~(CPU_BASED_CR3_LOAD_EXITING |
				CPU_BASED_CR3_STORE_EXITING |
				CPU_BASED_INTR_WINDOW_EXITING |
				CPU_BASED_NMI_WINDOW_EXITING);
	exec_controls_set(vmx, cpu_based_exec_ctrl);

	/* disable EPT/VPID first, enable after EPT pgtable created */
	cpu_based_2nd_exec_ctrl &= ~(SECONDARY_EXEC_ENABLE_EPT |
				SECONDARY_EXEC_ENABLE_VPID);
	/*
	 * SECONDARY_EXEC_SHADOW_VMCS is enabled when L1 executes VMPTRLD
	 * (handle_vmptrld).
	 * We can NOT enable shadow_vmcs here because we don't have yet
	 * a current VMCS12
	 */
	cpu_based_2nd_exec_ctrl &= ~SECONDARY_EXEC_SHADOW_VMCS;
	secondary_exec_controls_set(vmx, cpu_based_2nd_exec_ctrl);
	vmcs_write64(VMCS_LINK_POINTER, INVALID_GPA);

	/* guest owns cr3 */
	vmcs_write32(CR3_TARGET_COUNT, 0);

	/* guest handles exception directly */
	vmcs_write32(EXCEPTION_BITMAP, 0);

	vmcs_write64(IO_BITMAP_A, __pa(hvcpu->io_bitmap));
	vmcs_write64(IO_BITMAP_B, __pa(hvcpu->io_bitmap) + PAGE_SIZE);

	pkvm_sym(init_msr_emulation)(vmx);
	vmcs_write64(MSR_BITMAP, __pa(vmx->vmcs01.msr_bitmap));

	/*
	 * guest owns cr0, and owns cr4 except VMXE bit.
	 * does not care about IA32_VMX_CRx_FIXED0/1 setting, so if guest modify
	 * cr0/cr4 conflicting with FIXED0/1, just let #GP happen.
	 * For example, as pKVM does not enable unrestricted guest, cr0.PE/PG
	 * must keep as 1 in guest.
	 */
	vmcs_writel(CR0_GUEST_HOST_MASK, 0);
	vmcs_writel(CR4_GUEST_HOST_MASK, X86_CR4_VMXE);
}

static __init void init_vmexit_control(struct vcpu_vmx *vmx, struct vmcs_config *vmcs_config_ptr)
{
	vm_exit_controls_set(vmx, vmcs_config_ptr->vmexit_ctrl);
	vmcs_write32(VM_EXIT_MSR_STORE_COUNT, 0);
}

static __init void init_vmentry_control(struct vcpu_vmx *vmx, struct vmcs_config *vmcs_config_ptr)
{
	vm_entry_controls_set(vmx, vmcs_config_ptr->vmentry_ctrl);
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);
	vmcs_write32(VM_ENTRY_MSR_LOAD_COUNT, 0);
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);
}

static __init int pkvm_host_init_vmx(struct pkvm_host_vcpu *hvcpu, int cpu)
{
	struct vcpu_vmx *vmx = &hvcpu->vmx;
	int ret;

	ret = pkvm_enable_vmx(hvcpu);
	if (ret)
		return ret;

	/* vmcs01: host vmcs in pKVM */
	vmx->vmcs01.vmcs = pkvm_alloc_vmcs(&pkvm->vmcs_config);
	if (!vmx->vmcs01.vmcs)
		return -ENOMEM;

	vmx->vmcs01.msr_bitmap = pkvm_sym(pkvm_early_alloc_page)();
	if (!vmx->vmcs01.msr_bitmap) {
		pr_err("%s: No page for msr_bitmap\n", __func__);
		return -ENOMEM;
	}

	hvcpu->io_bitmap = pkvm->host_vm.io_bitmap;

	vmx->loaded_vmcs = &vmx->vmcs01;
	vmcs_load(vmx->loaded_vmcs->vmcs);

	init_guest_state_area(hvcpu, cpu);
	init_host_state_area(hvcpu, cpu);
	init_execution_control(hvcpu, &pkvm->vmcs_config, &pkvm->vmx_cap);
	init_vmexit_control(vmx, &pkvm->vmcs_config);
	init_vmentry_control(vmx, &pkvm->vmcs_config);

	return ret;
}

static __init void pkvm_host_deinit_vmx(struct pkvm_host_vcpu *hvcpu)
{
	struct vcpu_vmx *vmx = &hvcpu->vmx;

	pkvm_cpu_vmxoff();

	if (vmx->vmcs01.vmcs)
		vmx->vmcs01.vmcs = NULL;

	if (vmx->vmcs01.msr_bitmap)
		vmx->vmcs01.msr_bitmap = NULL;
}

static __init void pkvm_host_setup_nested_vmx_cap(struct pkvm_hyp *pkvm)
{
	struct nested_vmx_msrs *msrs = &pkvm->vmcs_config.nested;

	rdmsr(MSR_IA32_VMX_PROCBASED_CTLS,
		msrs->procbased_ctls_low,
		msrs->procbased_ctls_high);

	rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS2,
			&msrs->secondary_ctls_low,
			&msrs->secondary_ctls_high);

	rdmsr(MSR_IA32_VMX_PINBASED_CTLS,
		msrs->pinbased_ctls_low,
		msrs->pinbased_ctls_high);

	rdmsrl_safe(MSR_IA32_VMX_VMFUNC, &msrs->vmfunc_controls);

	rdmsr(MSR_IA32_VMX_EXIT_CTLS,
		msrs->exit_ctls_low,
		msrs->exit_ctls_high);

	rdmsr(MSR_IA32_VMX_ENTRY_CTLS,
		msrs->entry_ctls_low,
		msrs->entry_ctls_high);

	rdmsr(MSR_IA32_VMX_MISC, msrs->misc_low, msrs->misc_high);
}

static __init int pkvm_host_check_and_setup_vmx_cap(struct pkvm_hyp *pkvm)
{
	struct vmcs_config *vmcs_config = &pkvm->vmcs_config;
	struct vmx_capability *vmx_cap = &pkvm->vmx_cap;
	int ret = 0;
	struct vmcs_config_setting setting = {
		.cpu_based_vm_exec_ctrl_req =
			CPU_BASED_INTR_WINDOW_EXITING |
			CPU_BASED_USE_IO_BITMAPS |
			CPU_BASED_USE_MSR_BITMAPS |
			CPU_BASED_ACTIVATE_SECONDARY_CONTROLS,
		.cpu_based_vm_exec_ctrl_opt = 0,
		.secondary_vm_exec_ctrl_req =
			SECONDARY_EXEC_ENABLE_EPT |
			SECONDARY_EXEC_SHADOW_VMCS,
		.secondary_vm_exec_ctrl_opt =
			SECONDARY_EXEC_ENABLE_VPID |
			SECONDARY_EXEC_ENABLE_INVPCID |
			SECONDARY_EXEC_ENABLE_XSAVES |
			SECONDARY_EXEC_ENABLE_RDTSCP |
			SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE |
			SECONDARY_EXEC_EPT_VIOLATION_VE,
		.tertiary_vm_exec_ctrl_opt = 0,
		.pin_based_vm_exec_ctrl_req = 0,
		.pin_based_vm_exec_ctrl_opt = 0,
		.vmexit_ctrl_req =
			VM_EXIT_HOST_ADDR_SPACE_SIZE |
			VM_EXIT_LOAD_IA32_PAT |
			VM_EXIT_LOAD_IA32_EFER |
			VM_EXIT_SAVE_IA32_PAT |
			VM_EXIT_SAVE_IA32_EFER |
			VM_EXIT_SAVE_DEBUG_CONTROLS,
		.vmexit_ctrl_opt = 0,
		.vmentry_ctrl_req =
			VM_ENTRY_LOAD_DEBUG_CONTROLS |
			VM_ENTRY_IA32E_MODE |
			VM_ENTRY_LOAD_IA32_EFER |
			VM_ENTRY_LOAD_IA32_PAT,
		.vmentry_ctrl_opt = 0,
	};

	ret = setup_vmcs_config_common(vmcs_config, vmx_cap, &setting);
	if (ret) {
		pr_err("%s: fail with ret %d\n", __func__, ret);
	} else {
		pr_info("pin_based_exec_ctrl 0x%x\n", vmcs_config->pin_based_exec_ctrl);
		pr_info("cpu_based_exec_ctrl 0x%x\n", vmcs_config->cpu_based_exec_ctrl);
		pr_info("cpu_based_2nd_exec_ctrl 0x%x\n", vmcs_config->cpu_based_2nd_exec_ctrl);
		pr_info("vmexit_ctrl 0x%x\n", vmcs_config->vmexit_ctrl);
		pr_info("vmentry_ctrl 0x%x\n", vmcs_config->vmentry_ctrl);
	}

	pkvm_host_setup_nested_vmx_cap(pkvm);

	return ret;
}

static __init int pkvm_init_mmu(struct pkvm_hyp *pkvm)
{
	int pgsz_mask = (1 << PG_LEVEL_2M) | (1 << PG_LEVEL_4K);

	if (boot_cpu_has(X86_FEATURE_GBPAGES))
		pgsz_mask |= 1 << PG_LEVEL_1G;

	/* record mmu pgtable cap for later mmu pgtable build */
	pkvm->mmu_cap.level = pgtable_l5_enabled() ? 5 : 4;
	pkvm->mmu_cap.allowed_pgsz = pgsz_mask;
	pkvm->mmu_cap.table_prot = (u64)_KERNPG_TABLE_NOENC;

	/*
	 * Use IOMMU acknowledged level and page size mask for
	 * EPT as IOMMU will use EPT as its second-level page
	 * table in nested translation.
	 */
	pkvm->ept_cap.level = pkvm->ept_iommu_pgt_level;
	pkvm->ept_cap.allowed_pgsz = pkvm->ept_iommu_pgsz_mask;
	pkvm->ept_cap.table_prot = VMX_EPT_RWX_MASK;

	/*
	 * __page_base_offset stores the offset for pkvm
	 * to translate VA to a PA.
	 *
	 * __symbol_base_offset stores the offset for pkvm
	 * to translate its symbole's VA to a PA.
	 */
	pkvm_sym(__page_base_offset) = (unsigned long)__va(0);
	pkvm_sym(__symbol_base_offset) = (unsigned long)__pkvm_text_start - __pa_symbol(__pkvm_text_start);

	/*
	 * __x86_clflush_size stores the clflush size for
	 * pkvm to do the clfush at runtime.
	 */
	pkvm_sym(__x86_clflush_size) = boot_cpu_data.x86_clflush_size;

	return 0;
}

static __init void init_gdt(struct pkvm_pcpu *pcpu)
{
	pcpu->gdt_page = pkvm_gdt_page;
}

static __init void init_idt(struct pkvm_pcpu *pcpu)
{
	void (*pkvm_exception_handlers[X86_TRAP_IRET])(void) = {
#define GEN(x, ...)	\
		[x] = pkvm_sym(handle_exception_##x),
#include <GEN-for-each-exc.h>
#undef GEN
	};
	gate_desc *idt = pcpu->idt_page.idt;
	struct idt_data d = {
		.segment = __KERNEL_CS,
		.bits.ist = 0,
		.bits.zero = 0,
		.bits.type = GATE_INTERRUPT,
		.bits.dpl = 0,
		.bits.p = 1,
	};
	gate_desc desc;
	int i;

	for (i = 0; i < X86_TRAP_IRET; i++) {
		d.vector = i;
		d.bits.ist = 0;
		d.addr = (const void *)pkvm_exception_handlers[i];
		idt_init_desc(&desc, &d);
		write_idt_entry(idt, i, &desc);
	}
}

static __init void init_tss(struct pkvm_pcpu *pcpu)
{
	struct desc_struct *d = pcpu->gdt_page.gdt;
	tss_desc tss;

	set_tssldt_descriptor(&tss, (unsigned long)&pcpu->tss, DESC_TSS,
			__KERNEL_TSS_LIMIT);

	write_gdt_entry(d, GDT_ENTRY_TSS, &tss, DESC_TSS);
}

static __init int pkvm_setup_pcpu(struct pkvm_hyp *pkvm, int cpu)
{
	struct pkvm_pcpu *pcpu;
#ifndef CONFIG_PKVM_INTEL_DEBUG
	int nr_pages;
#endif

	if (cpu >= CONFIG_NR_CPUS)
		return -ENOMEM;

#ifndef CONFIG_PKVM_INTEL_DEBUG
	nr_pages = pkvm_sym(pkvm_per_cpu_nr_pages)();
	if (nr_pages) {
		void *per_cpu_base = pkvm_sym(pkvm_early_alloc_contig)(nr_pages);

		if (!per_cpu_base || pkvm_sym(setup_pkvm_per_cpu)(cpu, __pa(per_cpu_base))) {
			pr_err("%s: No page for pKVM per cpu data\n", __func__);
			return -ENOMEM;
		}
	}
#else
	/*
	 * Overwrite the pkvm's percpu setup symbols with the host percpu value
	 * as the same percpu base will be used by the pkvm and the host in the
	 * debug build.
	 */
	pkvm_sym(__per_cpu_offset)[cpu] = __per_cpu_offset[cpu];
	per_cpu(pkvm_sym(this_cpu_off), cpu) = __per_cpu_offset[cpu];
	per_cpu(pkvm_sym(pcpu_hot).cpu_number, cpu) = cpu;
#endif

	pcpu = pkvm_sym(pkvm_early_alloc_contig)(PKVM_PCPU_PAGES);
	if (!pcpu)
		return -ENOMEM;

	/* tmp use host cr3, switch to pkvm owned cr3 after de-privilege */
	pcpu->cr3 = __read_cr3();

	init_gdt(pcpu);
	init_idt(pcpu);
	init_tss(pcpu);

	pkvm->pcpus[cpu] = pcpu;

	return 0;
}

static __init int pkvm_host_setup_vcpu(struct pkvm_hyp *pkvm, int cpu)
{
	struct pkvm_host_vcpu *hvcpu;

	if (cpu >= CONFIG_NR_CPUS)
		return -ENOMEM;

	hvcpu = pkvm_sym(pkvm_early_alloc_contig)(PKVM_HOST_VCPU_PAGES);
	if (!hvcpu)
		return -ENOMEM;

	hvcpu->pcpu = pkvm->pcpus[cpu];
	hvcpu->vmx.vcpu.cpu = cpu;

	pkvm->host_vm.host_vcpus[cpu] = hvcpu;

	return 0;
}

static inline void enable_feature_control(void)
{
	u64 old, test_bits;

	rdmsrl(MSR_IA32_FEAT_CTL, old);
	test_bits = FEAT_CTL_LOCKED;
	test_bits |= FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX;

	if ((old & test_bits) != test_bits)
		wrmsrl(MSR_IA32_FEAT_CTL, old | test_bits);
}

static noinline int local_deprivilege_cpu(struct pkvm_host_vcpu *hvcpu)
{
	volatile int ret = 0;

	asm volatile(
		"pushfq\n"
		"popq %%rax\n"
		"movq %3, %%rdx\n"
		"vmwrite %%rax, %%rdx\n"
		"movq %%rsp, %%rax\n"
		"movq %4, %%rdx\n"
		"vmwrite %%rax, %%rdx\n"
		"movq $host_vm_entry_point, %%rax\n"
		"movq %1, %%rdx\n"
		"vmwrite %%rax, %%rdx\n"
		"vmlaunch \n"
		"ja host_vm_entry_point\n"              /* successfully deprivileged (CF=0 & ZF=0) */
		"movq %2, %%rax\n"			/* vmlaunch failed */
		"movq %%rax, %0\n"
		"host_vm_entry_point: nop\n"
		: "=m"(ret)
		: "i"(GUEST_RIP), "i"(-EINVAL), "i"(GUEST_RFLAGS), "i"(GUEST_RSP)
		: "rax", "rdx", "memory");

	return ret;
}

static __init void pkvm_host_deprivilege_cpu(void *data)
{
	struct pkvm_deprivilege_param *p = data;
	unsigned long flags;
	int cpu = get_cpu(), ret;
	struct pkvm_host_vcpu *hvcpu =
		p->pkvm->host_vm.host_vcpus[cpu];
	struct kvm_vcpu *vcpu;

	local_irq_save(flags);

	enable_feature_control();

	ret = pkvm_host_init_vmx(hvcpu, cpu);
	if (ret) {
		pr_err("%s: init vmx failed\n", __func__);
		goto out;
	}

	ret = local_deprivilege_cpu(hvcpu);
	if (ret == 0) {
		vcpu = &hvcpu->vmx.vcpu;
		vcpu->mode = IN_GUEST_MODE;
		pr_info("%s: CPU%d in guest mode\n", __func__, cpu);
		goto ok;
	}

out:
	p->ret = ret;
	pkvm_host_deinit_vmx(hvcpu);
	pr_err("%s: failed to deprivilege CPU%d\n", __func__, cpu);

ok:
	local_irq_restore(flags);

	put_cpu();
}

/*
 * Used in root mode to deprivilege CPUs
 */
static __init int pkvm_host_deprivilege_cpus(struct pkvm_hyp *pkvm)
{
	struct pkvm_deprivilege_param p = {
		.pkvm = pkvm,
		.ret = 0,
	};

	on_each_cpu(pkvm_host_deprivilege_cpu, &p, 1);
	if (p.ret) {
		/*
		 * TODO:
		 * We are here because some CPUs failed to be deprivileged, so
		 * the failed CPU will stay in root mode. But the others already
		 * in the non-root mode. In this case, we should let non-root mode
		 * CPUs go back to root mode, then the system can still run natively
		 * without pKVM enabled.
		 */
		pr_err("%s: WARNING - failed to deprivilege all CPUs!\n", __func__);
	} else {
		pr_info("%s: all cpus are in guest mode!\n", __func__);
	}

	return p.ret;
}

/*
 * Called with interrupts disabled, so we don't race with the IOMMU driver's
 * IRQ handler or any other kernel code that might run concurrently with pKVM
 * initialization and relies on an up-to-date value of pkvm_enabled.
 */
static int __this_cpu_do_finalise_hc(struct pkvm_section *sections, unsigned long size)
{
	int ret = kvm_hypercall2(PKVM_HC_INIT_FINALISE, (unsigned long)sections, size);
	if (!ret)
		this_cpu_write(pkvm_enabled, true);

	return ret;
}

/* Called with preemption disabled but interrupts enabled. */
static int this_cpu_do_finalise_hc(struct pkvm_section *sections, unsigned long size)
{
	int ret;

	local_irq_disable();
	ret = __this_cpu_do_finalise_hc(sections, size);
	local_irq_enable();

	return ret;
}

static __init void do_pkvm_finalise(void *data)
{
	__this_cpu_do_finalise_hc(NULL, 0);
}

static __init int pkvm_init_finalise(void)
{
	int ret, cpu;
	int self = get_cpu();
	struct pkvm_section sections[] = {
		/*
		 * NOTE: please ensure kernel section is put at the beginning,
		 * as we do section mapping by the order, while kernel data
		 * sections have overlap with pkvm ones, put the kernel section
		 * after pkvm one will make pkvm section readonly!
		 */
		{
			/*
			 * Kernel section: addr is virtual, needed
			 * for pkvm to access kernel alias symbol
			 */
			.type = KERNEL_DATA_SECTIONS,
			.addr = (unsigned long)_sdata,
			.size = (unsigned long)(_edata - _sdata),
			.prot = (u64)pgprot_val(PAGE_KERNEL_RO),
		},
		{
			/*
			 * Kernel section: addr is virtual, needed
			 * for pkvm to access kernel alias symbol
			 */
			.type = KERNEL_DATA_SECTIONS,
			.addr = (unsigned long)__start_rodata,
			.size = (unsigned long)(__end_rodata - __start_rodata),
			.prot = (u64)pgprot_val(PAGE_KERNEL_RO),
		},
		{
			/* PKVM reserved memory: addr is physical */
			.type = PKVM_RESERVED_MEMORY,
			.addr = (unsigned long)hyp_mem_base,
			.size = (unsigned long)hyp_mem_size,
			.prot = (u64)pgprot_val(PAGE_KERNEL),
		},
		{
			/* PKVM section: addr is virtual */
			.type = PKVM_CODE_DATA_SECTIONS,
			.addr = (unsigned long)__pkvm_text_start,
			.size = (unsigned long)(__pkvm_text_end - __pkvm_text_start),
			.prot = (u64)pgprot_val(PAGE_KERNEL_EXEC),
		},
		{
			/* PKVM section: addr is virtual */
			.type = PKVM_CODE_DATA_SECTIONS,
			.addr = (unsigned long)__pkvm_rodata_start,
			.size = (unsigned long)(__pkvm_rodata_end - __pkvm_rodata_start),
			.prot = (u64)pgprot_val(PAGE_KERNEL_RO),
		},
		{
			/* PKVM section: addr is virtual */
			.type = PKVM_CODE_DATA_SECTIONS,
			.addr = (unsigned long)__pkvm_data_start,
			.size = (unsigned long)(__pkvm_data_end - __pkvm_data_start),
			.prot = (u64)pgprot_val(PAGE_KERNEL),
		},
		{
			/* PKVM section: addr is virtual */
			.type = PKVM_CODE_DATA_SECTIONS,
			.addr = (unsigned long)__pkvm_bss_start,
			.size = (unsigned long)(__pkvm_bss_end - __pkvm_bss_start),
			.prot = (u64)pgprot_val(PAGE_KERNEL),
		},
	};

	/*
	 * First hypercall to recreate the pgtable for pkvm, and init
	 * memory pool for later use, on boot cpu.
	 * Input parameters are only needed for first hypercall.
	 */
	ret = this_cpu_do_finalise_hc(sections, ARRAY_SIZE(sections));
	if (ret) {
		pr_err("%s: pkvm finalise failed!\n", __func__);
		goto out;
	}

	for_each_possible_cpu(cpu) {
		if (cpu == self)
			continue;

		/*
		 * Second hypercall to switch the mmu and ept pgtable
		 * for other cpus other than boot cpu.
		 */
		ret = smp_call_function_single(cpu, do_pkvm_finalise,
					       NULL, true);
	}

	ret = kvm_hypercall0(PKVM_HC_ACTIVATE_IOMMU);
out:
	put_cpu();

	return ret;
}

static int add_device_to_pkvm(struct device *dev, void *data)
{
	struct kvm_protected_vm *pkvm = data;
	struct pci_dev *pdev;
	u16 devid;

	if (!dev_is_pci(dev))
		return 0;

	pdev = to_pci_dev(dev);
	devid = PCI_DEVID(pdev->bus->number, pdev->devfn);

	return kvm_hypercall3(PKVM_HC_ADD_PTDEV, pkvm->pkvm_vm_handle, devid, 0);
}

int kvm_arch_add_device_to_pkvm(struct kvm *kvm, struct iommu_group *grp)
{
	int ret = 0;

	kvm_get_kvm(kvm);

	if (pkvm_is_protected_vm(kvm))
		ret = iommu_group_for_each_dev(grp, &kvm->arch.pkvm,
					       add_device_to_pkvm);

	kvm_put_kvm(kvm);

	return ret;
}

static int __pkvm_tlb_remote_flush_with_range(struct kvm *kvm,
					      struct pkvm_tlb_range *range)
{
	int pkvm_vm_handle = kvm->arch.pkvm.pkvm_vm_handle;
	u64 start_gpa = 0;
	u64 size = 0;

	if (pkvm_vm_handle <= 0)
		return -EOPNOTSUPP;

	if (range) {
		start_gpa = range->start_gfn << PAGE_SHIFT;
		size = range->pages * PAGE_SIZE;
	}

	return kvm_hypercall3(PKVM_HC_TLB_REMOTE_FLUSH_RANGE,
			      pkvm_vm_handle, start_gpa, size);
}

int pkvm_tlb_remote_flush_with_range(struct kvm *kvm, gfn_t start_gfn, gfn_t nr_pages)
{
	struct pkvm_tlb_range range = {
		.start_gfn = start_gfn,
		.pages = nr_pages,
	};

	return __pkvm_tlb_remote_flush_with_range(kvm, &range);
}

int pkvm_tlb_remote_flush(struct kvm *kvm)
{
	return __pkvm_tlb_remote_flush_with_range(kvm, NULL);
}

int pkvm_set_mmio_ve(struct kvm_vcpu *vcpu, unsigned long gfn)
{
	if (pkvm_is_protected_vcpu(vcpu)) {
		int pkvm_vm_handle = vcpu->kvm->arch.pkvm.pkvm_vm_handle;
		int pkvm_vcpu_handle = vcpu->arch.pkvm_vcpu_handle;

		kvm_hypercall3(PKVM_HC_SET_MMIO_VE, pkvm_vm_handle,
			       pkvm_vcpu_handle, gfn);
		return 1;
	}

	return 0;
}

static int pkvm_vm_ioctl_set_fw_gpa(struct kvm *kvm, u64 gpa)
{
	struct kvm_protected_vm *pkvm = &kvm->arch.pkvm;
	int ret = 0;

	if (!cmdline_pvmfw_present)
		return -EINVAL;

	mutex_lock(&pkvm->finalized_lock);
	if (pkvm->finalized) {
		ret = -EBUSY;
		goto out;
	}
	pkvm->pvmfw_load_addr = gpa;
out:
	mutex_unlock(&pkvm->finalized_lock);
	return ret;
}

static int pkvm_vm_ioctl_info(struct kvm *kvm,
			      struct kvm_protected_vm_info __user *info)
{
	struct kvm_protected_vm_info kinfo = {
		.firmware_size = cmdline_pvmfw_present ?
				 cmdline_pvmfw_size :
				 0,
	};

	return copy_to_user(info, &kinfo, sizeof(kinfo)) ? -EFAULT : 0;
}

int pkvm_vm_ioctl_enable_cap(struct kvm *kvm, struct kvm_enable_cap *cap)
{
	if (!enable_pkvm)
		return -EINVAL;

	if (!pkvm_is_protected_vm(kvm))
		return -EINVAL;

	if (cap->args[1] || cap->args[2] || cap->args[3])
		return -EINVAL;

	switch (cap->flags) {
	case KVM_CAP_X86_PROTECTED_VM_FLAGS_SET_FW_GPA:
		return pkvm_vm_ioctl_set_fw_gpa(kvm, cap->args[0]);
	case KVM_CAP_X86_PROTECTED_VM_FLAGS_INFO:
		return pkvm_vm_ioctl_info(kvm, (void __force __user *)cap->args[0]);
	default:
		return -EINVAL;
	}
}

static __init int pkvm_init_io_emulation(struct pkvm_hyp *pkvm)
{
	pkvm->host_vm.io_bitmap = pkvm_sym(pkvm_early_alloc_contig)(2);

	if (!pkvm->host_vm.io_bitmap) {
		pr_err("pkvm: %s: No page for io_bitmap\n", __func__);
		return -ENOMEM;
	}

	memset(pkvm->host_vm.io_bitmap, 0, 2 * PAGE_SIZE);

	return 0;
}

static __init int pkvm_init_pci(struct pkvm_hyp *pkvm)
{
	struct pci_mmcfg_region *data, *cfg;
	int length = 0, max_region_num = PAGE_SIZE / sizeof(struct pci_mmcfg_region);

	data = pkvm_sym(pkvm_early_alloc_page)();

	list_for_each_entry_rcu(cfg, &pci_mmcfg_list, list, pci_mmcfg_lock_held()) {
		if (length >= max_region_num)
			return -ENOMEM;
		memcpy(&data[length], cfg, sizeof(struct pci_mmcfg_region));
		length += 1;
	}

	pkvm->host_vm.pci_info.mmcfg_table = data;
	pkvm->host_vm.pci_info.mmcfg_table_size = length;

	pkvm_sym(init_pci)(pkvm);

	return 0;
}

static int __init pkvm_firmware_rmem_init(void)
{
	phys_addr_t start, end, size;

	if (!cmdline_pvmfw_present)
		return 0;

	start = cmdline_pvmfw_base;
	end = cmdline_pvmfw_base + cmdline_pvmfw_size - 1;
	size = cmdline_pvmfw_size;

	if (!e820__mapped_all(start, end, E820_TYPE_RESERVED)) {
		pr_err("pkvm: pvmfw memory [0x%llx-0x%llx] is not reserved in e820\n",
		       start, end);
		return -EINVAL;
	}

	if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(size)) {
		pr_err("pkvm: pvmfw memory [0x%llx-0x%llx] is not page-aligned\n",
		       start, end);
		return -EINVAL;
	}

	pkvm_sym(pvmfw_present) = true;
	pkvm_sym(pvmfw_base) = start;
	pkvm_sym(pvmfw_size) = size;
	return 0;
}

static int __init pkvm_firmware_rmem_clear(void)
{
	void *addr;
	phys_addr_t size;

	if (!cmdline_pvmfw_present)
		return 0;

	size = cmdline_pvmfw_size;
	addr = memremap(cmdline_pvmfw_base, size, MEMREMAP_WB);
	if (!addr)
		return -EINVAL;

	memset(addr, 0, size);
	clflush_cache_range(addr, size);
	memunmap(addr);

	pr_info("pkvm: Cleared pvmfw memory\n");
	return 0;
}

static void __init setup_pkvm_syms(void)
{
	memcpy(&pkvm_sym(boot_cpu_data), &boot_cpu_data, sizeof(struct cpuinfo_x86));
	pkvm_sym(tsc_khz) = tsc_khz;
	cpumask_copy(&pkvm_sym(__cpu_possible_mask), cpu_possible_mask);
	pkvm_sym(nr_cpu_ids) = nr_cpu_ids;
	pkvm_sym(x86_pred_cmd) = x86_pred_cmd;
}

int __init vmx_pkvm_init(void)
{
	int ret = 0, cpu;

	ret = pkvm_firmware_rmem_init();
	if (ret)
		return ret;

	if (!enable_pkvm) {
		pkvm_firmware_rmem_clear();
		return -EOPNOTSUPP;
	}

	if (pkvm_sym(pkvm_hyp)) {
		pr_err("pkvm hypervisor is running!");
		return -EBUSY;
	}

	if (!hyp_mem_base) {
		pr_err("pkvm required memory not get reserved!");
		ret = -ENOMEM;
		goto out;
	}
	pkvm_sym(pkvm_early_alloc_init)(__va(hyp_mem_base),
			pkvm_data_struct_pages(PKVM_GLOBAL_PAGES,
					       PKVM_PERCPU_PAGES,
					       num_possible_cpus()) << PAGE_SHIFT);

	/* pkvm hypervisor keeps same VA mapping as deprivileged host */
	pkvm = pkvm_sym(pkvm_hyp) = pkvm_sym(pkvm_early_alloc_contig)(PKVM_PAGES);
	if (!pkvm) {
		ret = -ENOMEM;
		goto out;
	}

	setup_pkvm_syms();

	ret = pkvm_host_check_and_setup_vmx_cap(pkvm);
	if (ret)
		goto out;

	ret = check_and_init_iommu(pkvm);
	if (ret)
		goto out;

	ret = pkvm_init_mmu(pkvm);
	if (ret)
		goto out;

	ret = pkvm_init_io_emulation(pkvm);
	if (ret)
		goto out;

	ret = pkvm_init_pci(pkvm);
	if (ret)
		goto out;

	for_each_possible_cpu(cpu) {
		ret = pkvm_setup_pcpu(pkvm, cpu);
		if (ret)
			goto out;
		ret = pkvm_host_setup_vcpu(pkvm, cpu);
		if (ret)
			goto out;
	}

	ret = pkvm_host_deprivilege_cpus(pkvm);
	if (ret)
		goto out;

	pkvm->num_cpus = num_possible_cpus();
	pkvm_init_debugfs();

	ret = pkvm_init_finalise();
	if (ret)
		pkvm_firmware_rmem_clear();
	return ret;

out:
	pkvm_firmware_rmem_clear();
	pkvm_sym(pkvm_hyp) = NULL;
	/* TODO: Revisit if the memory resource may be reused here */
	return ret;
}
