/* SPDX-License-Identifier: GPL-2.0 */

#include <pkvm.h>
#include "cpu.h"

void pkvm_init_host_state_area(struct pkvm_pcpu *pcpu, int cpu)
{
	unsigned long a;
#ifdef CONFIG_PKVM_INTEL_DEBUG
	u32 high, low;
	struct desc_ptr dt;
	u16 selector;
#endif

	vmcs_writel(HOST_CR0, native_read_cr0() & ~X86_CR0_TS);
	vmcs_writel(HOST_CR3, pcpu->cr3);
	vmcs_writel(HOST_CR4, native_read_cr4());

#ifdef CONFIG_PKVM_INTEL_DEBUG
	savesegment(cs, selector);
	vmcs_write16(HOST_CS_SELECTOR, selector);
	savesegment(ss, selector);
	vmcs_write16(HOST_SS_SELECTOR, selector);
	savesegment(ds, selector);
	vmcs_write16(HOST_DS_SELECTOR, selector);
	savesegment(es, selector);
	vmcs_write16(HOST_ES_SELECTOR, selector);
	savesegment(fs, selector);
	vmcs_write16(HOST_FS_SELECTOR, selector);
	pkvm_rdmsrl(MSR_FS_BASE, a);
	vmcs_writel(HOST_FS_BASE, a);
	savesegment(gs, selector);
	vmcs_write16(HOST_GS_SELECTOR, selector);
	pkvm_rdmsrl(MSR_GS_BASE, a);
	vmcs_writel(HOST_GS_BASE, a);

	vmcs_write16(HOST_TR_SELECTOR, GDT_ENTRY_TSS*8);
	vmcs_writel(HOST_TR_BASE, (unsigned long)&get_cpu_entry_area(cpu)->tss.x86_tss);

	native_store_gdt(&dt);
	vmcs_writel(HOST_GDTR_BASE, dt.address);
	vmcs_writel(HOST_IDTR_BASE, (unsigned long)(&pcpu->idt_page));

	pkvm_rdmsr(MSR_IA32_SYSENTER_CS, low, high);
	vmcs_write32(HOST_IA32_SYSENTER_CS, low);

	pkvm_rdmsrl(MSR_IA32_SYSENTER_ESP, a);
	vmcs_writel(HOST_IA32_SYSENTER_ESP, a);

	pkvm_rdmsrl(MSR_IA32_SYSENTER_EIP, a);
	vmcs_writel(HOST_IA32_SYSENTER_EIP, a);
#else
	vmcs_write16(HOST_CS_SELECTOR, __KERNEL_CS);
	vmcs_write16(HOST_SS_SELECTOR, __KERNEL_DS);
	vmcs_write16(HOST_DS_SELECTOR, __KERNEL_DS);
	vmcs_write16(HOST_ES_SELECTOR, 0);
	vmcs_write16(HOST_TR_SELECTOR, GDT_ENTRY_TSS*8);
	vmcs_write16(HOST_FS_SELECTOR, 0);
	vmcs_write16(HOST_GS_SELECTOR, 0);
	vmcs_writel(HOST_FS_BASE, 0);
	vmcs_writel(HOST_GS_BASE, 0);

	vmcs_writel(HOST_TR_BASE, (unsigned long)&pcpu->tss);
	vmcs_writel(HOST_GDTR_BASE, (unsigned long)(&pcpu->gdt_page));
	vmcs_writel(HOST_IDTR_BASE, (unsigned long)(&pcpu->idt_page));

	vmcs_write16(HOST_GS_SELECTOR, __KERNEL_DS);
	vmcs_writel(HOST_GS_BASE, cpu);
#endif

	/* MSR area */
	pkvm_rdmsrl(MSR_EFER, a);
	vmcs_write64(HOST_IA32_EFER, a);

	pkvm_rdmsrl(MSR_IA32_CR_PAT, a);
	vmcs_write64(HOST_IA32_PAT, a);
}
