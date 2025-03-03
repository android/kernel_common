// SPDX-License-Identifier: GPL-2.0
#include <linux/array_size.h>
#include <vmx/vmx_ops.h>
#include <asm/cpuid.h>
#include <asm/msr.h>
#include <vmx/vmx.h>
#include <vmx/x86_ops.h>
#include "vmx.h"

static bool __read_mostly error_on_inconsistent_vmcs_config = true;

#define vmx_insn_failed(fmt...)		\
do {					\
	WARN_ONCE(1, fmt);		\
	pr_warn_ratelimited(fmt);	\
} while (0)

noinline void vmread_error(unsigned long field)
{
	vmx_insn_failed("vmread failed: field=%lx\n", field);
}

#ifndef CONFIG_CC_HAS_ASM_GOTO_OUTPUT
noinstr void vmread_error_trampoline2(unsigned long field, bool fault)
{
	if (fault) {
		kvm_spurious_fault();
	} else {
		instrumentation_begin();
		vmread_error(field);
		instrumentation_end();
	}
}
#endif

noinline void vmwrite_error(unsigned long field, unsigned long value)
{
	vmx_insn_failed("vmwrite failed: field=%lx val=%lx err=%u\n",
			field, value, vmcs_read32(VM_INSTRUCTION_ERROR));
}

noinline void vmclear_error(struct vmcs *vmcs, u64 phys_addr)
{
	vmx_insn_failed("vmclear failed: %p/%llx err=%u\n",
			vmcs, phys_addr, vmcs_read32(VM_INSTRUCTION_ERROR));
}

noinline void vmptrld_error(struct vmcs *vmcs, u64 phys_addr)
{
	vmx_insn_failed("vmptrld failed: %p/%llx err=%u\n",
			vmcs, phys_addr, vmcs_read32(VM_INSTRUCTION_ERROR));
}

noinline void invvpid_error(unsigned long ext, u16 vpid, gva_t gva)
{
	vmx_insn_failed("invvpid failed: ext=0x%lx vpid=%u gva=0x%lx\n",
			ext, vpid, gva);
}

noinline void invept_error(unsigned long ext, u64 eptp, gpa_t gpa)
{
	vmx_insn_failed("invept failed: ext=0x%lx eptp=%llx gpa=0x%llx\n",
			ext, eptp, gpa);
}

struct vmcs_config vmcs_config __ro_after_init;
struct vmx_capability vmx_capability __ro_after_init;

/*
 * Comment's format: document - errata name - stepping - processor name.
 * Refer from
 * https://www.virtualbox.org/svn/vbox/trunk/src/VBox/VMM/VMMR0/HMR0.cpp
 */
static u32 vmx_preemption_cpu_tfms[] = {
/* 323344.pdf - BA86   - D0 - Xeon 7500 Series */
0x000206E6,
/* 323056.pdf - AAX65  - C2 - Xeon L3406 */
/* 322814.pdf - AAT59  - C2 - i7-600, i5-500, i5-400 and i3-300 Mobile */
/* 322911.pdf - AAU65  - C2 - i5-600, i3-500 Desktop and Pentium G6950 */
0x00020652,
/* 322911.pdf - AAU65  - K0 - i5-600, i3-500 Desktop and Pentium G6950 */
0x00020655,
/* 322373.pdf - AAO95  - B1 - Xeon 3400 Series */
/* 322166.pdf - AAN92  - B1 - i7-800 and i5-700 Desktop */
/*
 * 320767.pdf - AAP86  - B1 -
 * i7-900 Mobile Extreme, i7-800 and i7-700 Mobile
 */
0x000106E5,
/* 321333.pdf - AAM126 - C0 - Xeon 3500 */
0x000106A0,
/* 321333.pdf - AAM126 - C1 - Xeon 3500 */
0x000106A1,
/* 320836.pdf - AAJ124 - C0 - i7-900 Desktop Extreme and i7-900 Desktop */
0x000106A4,
 /* 321333.pdf - AAM126 - D0 - Xeon 3500 */
 /* 321324.pdf - AAK139 - D0 - Xeon 5500 */
 /* 320836.pdf - AAJ124 - D0 - i7-900 Extreme and i7-900 Desktop */
0x000106A5,
 /* Xeon E3-1220 V2 */
0x000306A8,
};

static inline bool cpu_has_broken_vmx_preemption_timer(void)
{
	u32 eax = cpuid_eax(0x00000001), i;

	/* Clear the reserved bits */
	eax &= ~(0x3U << 14 | 0xfU << 28);
	for (i = 0; i < ARRAY_SIZE(vmx_preemption_cpu_tfms); i++)
		if (eax == vmx_preemption_cpu_tfms[i])
			return true;

	return false;
}

/*
 * There is no X86_FEATURE for SGX yet, but anyway we need to query CPUID
 * directly instead of going through cpu_has(), to ensure KVM is trapping
 * ENCLS whenever it's supported in hardware.  It does not matter whether
 * the host OS supports or has enabled SGX.
 */
static bool cpu_has_sgx(void)
{
	return cpuid_eax(0) >= 0x12 && (cpuid_eax(0x12) & BIT(0));
}

static int adjust_vmx_controls(u32 ctl_min, u32 ctl_opt, u32 msr, u32 *result)
{
	u32 vmx_msr_low, vmx_msr_high;
	u32 ctl = ctl_min | ctl_opt;

	rdmsr(msr, vmx_msr_low, vmx_msr_high);

	ctl &= vmx_msr_high; /* bit == 0 in high word ==> must be zero */
	ctl |= vmx_msr_low;  /* bit == 1 in low word  ==> must be one  */

	/* Ensure minimum (required) set of control bits are supported. */
	if (ctl_min & ~ctl)
		return -EIO;

	*result = ctl;
	return 0;
}

static u64 adjust_vmx_controls64(u64 ctl_opt, u32 msr)
{
	u64 allowed;

	rdmsrl(msr, allowed);

	return  ctl_opt & allowed;
}

int setup_vmcs_config_common(struct vmcs_config *vmcs_conf,
			     struct vmx_capability *vmx_cap,
			     struct vmcs_config_setting *setting)
{
	u32 _pin_based_exec_control = 0;
	u32 _cpu_based_exec_control = 0;
	u32 _cpu_based_2nd_exec_control = 0;
	u64 _cpu_based_3rd_exec_control = 0;
	u32 _vmexit_control = 0;
	u32 _vmentry_control = 0;
	u64 basic_msr;
	u64 misc_msr;

	memset(vmcs_conf, 0, sizeof(*vmcs_conf));

	if (adjust_vmx_controls(setting->cpu_based_vm_exec_ctrl_req,
				setting->cpu_based_vm_exec_ctrl_opt,
				MSR_IA32_VMX_PROCBASED_CTLS,
				&_cpu_based_exec_control))
		return -EIO;
	if (_cpu_based_exec_control & CPU_BASED_ACTIVATE_SECONDARY_CONTROLS) {
		if (adjust_vmx_controls(setting->secondary_vm_exec_ctrl_req,
					setting->secondary_vm_exec_ctrl_opt,
					MSR_IA32_VMX_PROCBASED_CTLS2,
					&_cpu_based_2nd_exec_control))
			return -EIO;
	}
	if (!IS_ENABLED(CONFIG_KVM_INTEL_PROVE_VE))
		_cpu_based_2nd_exec_control &= ~SECONDARY_EXEC_EPT_VIOLATION_VE;

#ifndef CONFIG_X86_64
	if (!(_cpu_based_2nd_exec_control &
				SECONDARY_EXEC_VIRTUALIZE_APIC_ACCESSES))
		_cpu_based_exec_control &= ~CPU_BASED_TPR_SHADOW;
#endif

	if (!(_cpu_based_exec_control & CPU_BASED_TPR_SHADOW))
		_cpu_based_2nd_exec_control &= ~(
				SECONDARY_EXEC_APIC_REGISTER_VIRT |
				SECONDARY_EXEC_VIRTUALIZE_X2APIC_MODE |
				SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY);

	rdmsr_safe(MSR_IA32_VMX_EPT_VPID_CAP,
		&vmx_cap->ept, &vmx_cap->vpid);

	if (!(_cpu_based_2nd_exec_control & SECONDARY_EXEC_ENABLE_EPT) &&
	    vmx_cap->ept) {
		pr_warn_once("EPT CAP should not exist if not support "
				"1-setting enable EPT VM-execution control\n");

		if (error_on_inconsistent_vmcs_config)
			return -EIO;

		vmx_cap->ept = 0;
		_cpu_based_2nd_exec_control &= ~SECONDARY_EXEC_EPT_VIOLATION_VE;
	}
	if (!(_cpu_based_2nd_exec_control & SECONDARY_EXEC_ENABLE_VPID) &&
	    vmx_cap->vpid) {
		pr_warn_once("VPID CAP should not exist if not support "
				"1-setting enable VPID VM-execution control\n");

		if (error_on_inconsistent_vmcs_config)
			return -EIO;

		vmx_cap->vpid = 0;
	}

	if (!cpu_has_sgx())
		_cpu_based_2nd_exec_control &= ~SECONDARY_EXEC_ENCLS_EXITING;

	if (_cpu_based_exec_control & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS)
		_cpu_based_3rd_exec_control =
			adjust_vmx_controls64(setting->tertiary_vm_exec_ctrl_opt,
					      MSR_IA32_VMX_PROCBASED_CTLS3);

	if (adjust_vmx_controls(setting->vmexit_ctrl_req,
				setting->vmexit_ctrl_opt,
				MSR_IA32_VMX_EXIT_CTLS,
				&_vmexit_control))
		return -EIO;

	if (adjust_vmx_controls(setting->pin_based_vm_exec_ctrl_req,
				setting->pin_based_vm_exec_ctrl_opt,
				MSR_IA32_VMX_PINBASED_CTLS,
				&_pin_based_exec_control))
		return -EIO;

	if (cpu_has_broken_vmx_preemption_timer())
		_pin_based_exec_control &= ~PIN_BASED_VMX_PREEMPTION_TIMER;
	if (!(_cpu_based_2nd_exec_control &
		SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY))
		_pin_based_exec_control &= ~PIN_BASED_POSTED_INTR;

	if (adjust_vmx_controls(setting->vmentry_ctrl_req,
				setting->vmentry_ctrl_opt,
				MSR_IA32_VMX_ENTRY_CTLS,
				&_vmentry_control))
		return -EIO;

	rdmsrl(MSR_IA32_VMX_BASIC, basic_msr);

	/* IA-32 SDM Vol 3B: VMCS size is never greater than 4kB. */
	if (vmx_basic_vmcs_size(basic_msr) > PAGE_SIZE)
		return -EIO;

#ifdef CONFIG_X86_64
	/*
	 * KVM expects to be able to shove all legal physical addresses into
	 * VMCS fields for 64-bit kernels, and per the SDM, "This bit is always
	 * 0 for processors that support Intel 64 architecture".
	 */
	if (basic_msr & VMX_BASIC_32BIT_PHYS_ADDR_ONLY)
		return -EIO;
#endif

	/* Require Write-Back (WB) memory type for VMCS accesses. */
	if (vmx_basic_vmcs_mem_type(basic_msr) != X86_MEMTYPE_WB)
		return -EIO;

	rdmsrl(MSR_IA32_VMX_MISC, misc_msr);

	vmcs_conf->basic = basic_msr;
	vmcs_conf->pin_based_exec_ctrl = _pin_based_exec_control;
	vmcs_conf->cpu_based_exec_ctrl = _cpu_based_exec_control;
	vmcs_conf->cpu_based_2nd_exec_ctrl = _cpu_based_2nd_exec_control;
	vmcs_conf->cpu_based_3rd_exec_ctrl = _cpu_based_3rd_exec_control;
	vmcs_conf->vmexit_ctrl         = _vmexit_control;
	vmcs_conf->vmentry_ctrl        = _vmentry_control;
	vmcs_conf->misc	= misc_msr;

#if IS_ENABLED(CONFIG_HYPERV) && !defined(__PKVM_HYP__)
	if (enlightened_vmcs)
		evmcs_sanitize_exec_ctrls(vmcs_conf);
#endif

	return 0;
}

static int setup_vmcs_config(struct vmcs_config *vmcs_conf, struct vmx_capability *vmx_cap)
{
	int i, ret;
	struct vmcs_config_setting setting = {
		.cpu_based_vm_exec_ctrl_req = KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL,
		.cpu_based_vm_exec_ctrl_opt = KVM_OPTIONAL_VMX_CPU_BASED_VM_EXEC_CONTROL,
		.secondary_vm_exec_ctrl_req = KVM_REQUIRED_VMX_SECONDARY_VM_EXEC_CONTROL,
		.secondary_vm_exec_ctrl_opt = KVM_OPTIONAL_VMX_SECONDARY_VM_EXEC_CONTROL,
		.tertiary_vm_exec_ctrl_opt = KVM_OPTIONAL_VMX_TERTIARY_VM_EXEC_CONTROL,
		.pin_based_vm_exec_ctrl_req = KVM_REQUIRED_VMX_PIN_BASED_VM_EXEC_CONTROL,
		.pin_based_vm_exec_ctrl_opt = KVM_OPTIONAL_VMX_PIN_BASED_VM_EXEC_CONTROL,
		.vmexit_ctrl_req = KVM_REQUIRED_VMX_VM_EXIT_CONTROLS,
		.vmexit_ctrl_opt = KVM_OPTIONAL_VMX_VM_EXIT_CONTROLS,
		.vmentry_ctrl_req = KVM_REQUIRED_VMX_VM_ENTRY_CONTROLS,
		.vmentry_ctrl_opt = KVM_OPTIONAL_VMX_VM_ENTRY_CONTROLS,
	};

	/*
	 * LOAD/SAVE_DEBUG_CONTROLS are absent because both are mandatory.
	 * SAVE_IA32_PAT and SAVE_IA32_EFER are absent because KVM always
	 * intercepts writes to PAT and EFER, i.e. never enables those controls.
	 */
	struct {
		u32 entry_control;
		u32 exit_control;
	} const vmcs_entry_exit_pairs[] = {
		{ VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL,	VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL },
		{ VM_ENTRY_LOAD_IA32_PAT,		VM_EXIT_LOAD_IA32_PAT },
		{ VM_ENTRY_LOAD_IA32_EFER,		VM_EXIT_LOAD_IA32_EFER },
		{ VM_ENTRY_LOAD_BNDCFGS,		VM_EXIT_CLEAR_BNDCFGS },
		{ VM_ENTRY_LOAD_IA32_RTIT_CTL,		VM_EXIT_CLEAR_IA32_RTIT_CTL },
	};

	ret =  setup_vmcs_config_common(vmcs_conf, vmx_cap, &setting);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(vmcs_entry_exit_pairs); i++) {
		u32 n_ctrl = vmcs_entry_exit_pairs[i].entry_control;
		u32 x_ctrl = vmcs_entry_exit_pairs[i].exit_control;

		if (!(vmcs_conf->vmentry_ctrl & n_ctrl) == !(vmcs_conf->vmexit_ctrl & x_ctrl))
			continue;

		pr_warn_once("Inconsistent VM-Entry/VM-Exit pair, entry = %x, exit = %x\n",
				vmcs_conf->vmentry_ctrl & n_ctrl, vmcs_conf->vmexit_ctrl & x_ctrl);

		if (error_on_inconsistent_vmcs_config)
			return -EIO;

		vmcs_conf->vmentry_ctrl &= ~n_ctrl;
		vmcs_conf->vmexit_ctrl &= ~x_ctrl;
	}

	return ret;
}

__init int vmx_hardware_setup(void)
{
	return 0;
}

#ifdef __PKVM_HYP__
struct kvm_x86_ops vt_x86_ops __initdata = {
	.name = KBUILD_MODNAME,
};

struct kvm_x86_init_ops vt_init_ops __initdata = {
	.hardware_setup = vmx_hardware_setup,
	.runtime_ops = &vt_x86_ops,
};

int setup_vmx(void)
{
	int ret = setup_vmcs_config(&vmcs_config, &vmx_capability);

	if (ret)
		return ret;

	return kvm_x86_vendor_init(&vt_init_ops);
}
#endif
