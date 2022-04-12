// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/trapnr.h>

#include <pkvm.h>

MODULE_LICENSE("GPL");

static struct pkvm_hyp *pkvm;

struct pkvm_deprivilege_param {
	struct pkvm_hyp *pkvm;
	int ret;
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

static void *pkvm_early_alloc_contig(int pages)
{
	return alloc_pages_exact(pages << PAGE_SHIFT, GFP_KERNEL | __GFP_ZERO);
}

static void pkvm_early_free(void *ptr, int pages)
{
	free_pages_exact(ptr, pages << PAGE_SHIFT);
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

static __init int pkvm_enable_vmx(struct pkvm_host_vcpu *vcpu)
{
	u64 phys_addr;

	vcpu->vmxarea = pkvm_early_alloc_contig(1);
	if (!vcpu->vmxarea)
		return -ENOMEM;

	phys_addr = __pa(vcpu->vmxarea);
	if (!is_aligned(phys_addr, PAGE_SIZE))
		return -ENOMEM;

	/*setup revision id in vmxon region*/
	vmxon_setup_revid(vcpu->vmxarea);

	return pkvm_cpu_vmxon(phys_addr);
}

static __init int pkvm_host_init_vmx(struct pkvm_host_vcpu *vcpu)
{
	return pkvm_enable_vmx(vcpu);
}

static __init void pkvm_host_deinit_vmx(struct pkvm_host_vcpu *vcpu)
{
	pkvm_cpu_vmxoff();
}

static __init int pkvm_host_check_and_setup_vmx_cap(struct pkvm_hyp *pkvm)
{
	struct vmcs_config *vmcs_config = &pkvm->vmcs_config;
	struct vmx_capability *vmx_cap = &pkvm->vmx_cap;
	int ret = 0;
	struct vmcs_config_setting setting = {
		.cpu_based_vm_exec_ctrl_req =
			CPU_BASED_INTR_WINDOW_EXITING |
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
			SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE,
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

	return ret;
}

static __init void init_gdt(struct pkvm_pcpu *pcpu)
{
	pcpu->gdt_page = pkvm_gdt_page;
}

void noop_handler(void)
{
	/* To be added */
}

static __init void init_idt(struct pkvm_pcpu *pcpu)
{
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

	for (i = 0; i <= X86_TRAP_IRET; i++) {
		d.vector = i;
		d.bits.ist = 0;
		d.addr = (const void *)noop_handler;
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

	if (cpu >= CONFIG_NR_CPUS)
		return -ENOMEM;

	pcpu = pkvm_early_alloc_contig(PKVM_PCPU_PAGES);
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
	struct pkvm_host_vcpu *pkvm_host_vcpu;

	if (cpu >= CONFIG_NR_CPUS)
		return -ENOMEM;

	pkvm_host_vcpu = pkvm_early_alloc_contig(PKVM_HOST_VCPU_PAGES);
	if (!pkvm_host_vcpu)
		return -ENOMEM;

	pkvm_host_vcpu->pcpu = pkvm->pcpus[cpu];
	pkvm_host_vcpu->vmx.vcpu.cpu = cpu;

	pkvm->host_vm.host_vcpus[cpu] = pkvm_host_vcpu;

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

static __init void pkvm_host_deprivilege_cpu(void *data)
{
	struct pkvm_deprivilege_param *p = data;
	unsigned long flags;
	int cpu = get_cpu(), ret;
	struct pkvm_host_vcpu *vcpu =
		p->pkvm->host_vm.host_vcpus[cpu];

	local_irq_save(flags);

	enable_feature_control();

	ret = pkvm_host_init_vmx(vcpu);
	if (ret) {
		pr_err("%s: init vmx failed\n", __func__);
		goto out;
	}

	/* TODO:KICK to RUN vcpu. let's directly go with out(return failure) now */

out:
	p->ret = -ENOTSUPP;
	pkvm_host_deinit_vmx(vcpu);
	pr_err("%s: failed to deprivilege CPU%d\n", __func__, cpu);

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

__init int pkvm_init(void)
{
	int ret = 0, cpu;

	pkvm = pkvm_early_alloc_contig(PKVM_PAGES);
	if (!pkvm) {
		ret = -ENOMEM;
		goto out;
	}

	ret = pkvm_host_check_and_setup_vmx_cap(pkvm);
	if (ret)
		goto out_free_pkvm;

	for_each_possible_cpu(cpu) {
		ret = pkvm_setup_pcpu(pkvm, cpu);
		if (ret)
			goto out_free_cpu;
		ret = pkvm_host_setup_vcpu(pkvm, cpu);
		if (ret)
			goto out_free_cpu;
	}

	pkvm->num_cpus = num_possible_cpus();

	return 0;

out_free_cpu:
	for_each_possible_cpu(cpu) {
		if (pkvm->host_vm.host_vcpus[cpu]) {
			pkvm_early_free(pkvm->host_vm.host_vcpus[cpu], PKVM_HOST_VCPU_PAGES);
			pkvm->host_vm.host_vcpus[cpu] = NULL;
		}
		if (pkvm->pcpus[cpu]) {
			pkvm_early_free(pkvm->pcpus[cpu], PKVM_PCPU_PAGES);
			pkvm->pcpus[cpu] = NULL;
		}
	}
out_free_pkvm:
	pkvm_early_free(pkvm, PKVM_PAGES);
out:
	return ret;
}
