// SPDX-License-Identifier: GPL-2.0
#include <linux/array_size.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <vmx/vmx_ops.h>
#include <asm/cpuid.h>
#include <asm/msr.h>
#include <vmx/vmx.h>
#include <vmx/x86_ops.h>
#include <vmx/nested.h>
#include <vmx/sgx.h>
#include "vmx.h"
#include <pkvm/pkvm.h>

#ifdef __PKVM_HYP__
#undef module_param_named
#define module_param_named(...)
#endif

bool __read_mostly enable_vpid = 1;
module_param_named(vpid, enable_vpid, bool, 0444);

static bool __read_mostly enable_vnmi = 1;
module_param_named(vnmi, enable_vnmi, bool, 0444);

bool __read_mostly flexpriority_enabled = 1;
module_param_named(flexpriority, flexpriority_enabled, bool, 0444);

bool __read_mostly enable_ept = 1;
module_param_named(ept, enable_ept, bool, 0444);

bool __read_mostly enable_unrestricted_guest = 1;
module_param_named(unrestricted_guest,
			enable_unrestricted_guest, bool, 0444);
bool __read_mostly enable_ept_ad_bits = 1;
module_param_named(eptad, enable_ept_ad_bits, bool, 0444);

bool __read_mostly enable_ipiv = true;
module_param(enable_ipiv, bool, 0444);

/*
 * If nested=1, nested virtualization is supported, i.e., guests may use
 * VMX and be a hypervisor for its own guests. If nested=0, guests may not
 * use VMX instructions.
 */
static bool __read_mostly nested = 1;
module_param(nested, bool, 0444);

bool __read_mostly enable_pml = 1;
module_param_named(pml, enable_pml, bool, 0444);

static bool __read_mostly error_on_inconsistent_vmcs_config = true;
module_param(error_on_inconsistent_vmcs_config, bool, 0444);

#define KVM_VMX_TSC_MULTIPLIER_MAX     0xffffffffffffffffULL

/* Guest_tsc -> host_tsc conversion requires 64-bit division.  */
static int __read_mostly cpu_preemption_timer_multi;
static bool __read_mostly enable_preemption_timer = 1;
#ifdef CONFIG_X86_64
module_param_named(preemption_timer, enable_preemption_timer, bool, S_IRUGO);
#endif

/*
 * These 2 parameters are used to config the controls for Pause-Loop Exiting:
 * ple_gap:    upper bound on the amount of time between two successive
 *             executions of PAUSE in a loop. Also indicate if ple enabled.
 *             According to test, this time is usually smaller than 128 cycles.
 * ple_window: upper bound on the amount of time a guest is allowed to execute
 *             in a PAUSE loop. Tests indicate that most spinlocks are held for
 *             less than 2^12 cycles
 * Time is measured based on a counter that runs at the same rate as the TSC,
 * refer SDM volume 3b section 21.6.13 & 22.1.3.
 */
static unsigned int ple_gap = KVM_DEFAULT_PLE_GAP;
module_param(ple_gap, uint, 0444);

static unsigned int ple_window = KVM_VMX_DEFAULT_PLE_WINDOW;
module_param(ple_window, uint, 0444);

/* Default doubles per-vcpu window every exit. */
static unsigned int ple_window_grow = KVM_DEFAULT_PLE_WINDOW_GROW;
module_param(ple_window_grow, uint, 0444);

/* Default resets per-vcpu window every exit to ple_window. */
static unsigned int ple_window_shrink = KVM_DEFAULT_PLE_WINDOW_SHRINK;
module_param(ple_window_shrink, uint, 0444);

/* Default is to compute the maximum so we can never overflow. */
static unsigned int ple_window_max        = KVM_VMX_DEFAULT_PLE_WINDOW_MAX;
module_param(ple_window_max, uint, 0444);

/* Default is SYSTEM mode, 1 for host-guest mode */
int __read_mostly pt_mode = PT_MODE_SYSTEM;
#ifdef CONFIG_BROKEN
module_param(pt_mode, int, S_IRUGO);
#endif

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

DEFINE_PER_CPU(struct vmcs *, current_vmcs);
/*
 * We maintain a per-CPU linked-list of VMCS loaded on that CPU. This is needed
 * when a CPU is brought down, and we need to VMCLEAR all VMCSs loaded on it.
 */
static DEFINE_PER_CPU(struct list_head, loaded_vmcss_on_cpu);

static DECLARE_BITMAP(vmx_vpid_bitmap, VMX_NR_VPIDS);

struct vmcs_config vmcs_config __ro_after_init;
struct vmx_capability vmx_capability __ro_after_init;

static unsigned long host_idt_base;

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

static void __loaded_vmcs_clear(void *arg)
{
	struct loaded_vmcs *loaded_vmcs = arg;
	int cpu = raw_smp_processor_id();

	if (loaded_vmcs->cpu != cpu)
		return; /* vcpu migration can race with cpu offline */
	if (per_cpu(current_vmcs, cpu) == loaded_vmcs->vmcs)
		per_cpu(current_vmcs, cpu) = NULL;

	vmcs_clear(loaded_vmcs->vmcs);
	if (loaded_vmcs->shadow_vmcs && loaded_vmcs->launched)
		vmcs_clear(loaded_vmcs->shadow_vmcs);

	list_del(&loaded_vmcs->loaded_vmcss_on_cpu_link);

	/*
	 * Ensure all writes to loaded_vmcs, including deleting it from its
	 * current percpu list, complete before setting loaded_vmcs->cpu to
	 * -1, otherwise a different cpu can see loaded_vmcs->cpu == -1 first
	 * and add loaded_vmcs to its percpu list before it's deleted from this
	 * cpu's list. Pairs with the smp_rmb() in vmx_vcpu_load_vmcs().
	 */
	smp_wmb();

	loaded_vmcs->cpu = -1;
	loaded_vmcs->launched = 0;
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

/*
 * Some cpus support VM_{ENTRY,EXIT}_IA32_PERF_GLOBAL_CTRL but they
 * can't be used due to errata where VM Exit may incorrectly clear
 * IA32_PERF_GLOBAL_CTRL[34:32]. Work around the errata by using the
 * MSR load mechanism to switch IA32_PERF_GLOBAL_CTRL.
 */
static bool cpu_has_perf_global_ctrl_bug(void)
{
	switch (boot_cpu_data.x86_vfm) {
	case INTEL_NEHALEM_EP:	/* AAK155 */
	case INTEL_NEHALEM:	/* AAP115 */
	case INTEL_WESTMERE:	/* AAT100 */
	case INTEL_WESTMERE_EP:	/* BC86,AAY89,BD102 */
	case INTEL_NEHALEM_EX:	/* BA97 */
		return true;
	default:
		break;
	}

	return false;
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

int vmx_enable_virtualization_cpu(void)
{
#ifdef __PKVM_HYP__
	struct vcpu_vmx *vmx = to_vmx(this_cpu_read(host_vcpu));

	/*
	 * FIXME: As currently the PV ABIs are not fully functional, the
	 * emulation method is still required. Once PV ABIs are ready then these
	 * can reimplemented.
	 */
	if (vmx->nested.vmxon)
		return -EAGAIN;

	vmx->nested.current_vmptr = INVALID_GPA;
	vmx->nested.dirty_vmcs12 = false;
	vmx->nested.vmxon = true;

	return 0;
#else
	int cpu = raw_smp_processor_id();
	u64 phys_addr = __pa(per_cpu(vmxarea, cpu));
	int r;

	if (cr4_read_shadow() & X86_CR4_VMXE)
		return -EBUSY;

	/*
	 * This can happen if we hot-added a CPU but failed to allocate
	 * VP assist page for it.
	 */
	if (kvm_is_using_evmcs() && !hv_get_vp_assist_page(cpu))
		return -EFAULT;

	intel_pt_handle_vmx(1);

	r = kvm_cpu_vmxon(phys_addr);
	if (r) {
		intel_pt_handle_vmx(0);
		return r;
	}

	return 0;
#endif
}

static void vmclear_local_loaded_vmcss(void)
{
	int cpu = raw_smp_processor_id();
	struct loaded_vmcs *v, *n;

	list_for_each_entry_safe(v, n, &per_cpu(loaded_vmcss_on_cpu, cpu),
				 loaded_vmcss_on_cpu_link)
		__loaded_vmcs_clear(v);
}

void vmx_disable_virtualization_cpu(void)
{
#ifdef __PKVM_HYP__
	struct vcpu_vmx *vmx = to_vmx(this_cpu_read(host_vcpu));

	vmclear_local_loaded_vmcss();

	/*
	 * FIXME: As currently the PV ABIs are not fully functional, the
	 * emulation method is still required. Once PV ABIs are ready then these
	 * can reimplemented.
	 */
	vmx->nested.current_vmptr = INVALID_GPA;
	vmx->nested.dirty_vmcs12 = false;
	vmx->nested.vmxon = false;
#else
	vmclear_local_loaded_vmcss();

	if (kvm_cpu_vmxoff())
		kvm_spurious_fault();

	hv_reset_evmcs();

	intel_pt_handle_vmx(0);
#endif
}

static __init int alloc_kvm_area(void)
{
#ifndef __PKVM_HYP__
	int cpu;

	for_each_possible_cpu(cpu) {
		struct vmcs *vmcs;

		vmcs = alloc_vmcs_cpu(false, cpu, GFP_KERNEL);
		if (!vmcs) {
			free_kvm_area();
			return -ENOMEM;
		}

		/*
		 * When eVMCS is enabled, alloc_vmcs_cpu() sets
		 * vmcs->revision_id to KVM_EVMCS_VERSION instead of
		 * revision_id reported by MSR_IA32_VMX_BASIC.
		 *
		 * However, even though not explicitly documented by
		 * TLFS, VMXArea passed as VMXON argument should
		 * still be marked with revision_id reported by
		 * physical CPU.
		 */
		if (kvm_is_using_evmcs())
			vmcs->hdr.revision_id = vmx_basic_vmcs_revision_id(vmcs_config.basic);

		per_cpu(vmxarea, cpu) = vmcs;
	}
#endif
	return 0;
}

/*
 * The exit handlers return 1 if the exit was handled fully and guest execution
 * may resume.  Otherwise they set the kvm_run parameter to indicate what needs
 * to be done to userspace and return 0.
 */
static int (*kvm_vmx_exit_handlers[])(struct kvm_vcpu *vcpu) = {
};

static __init u64 vmx_get_perf_capabilities(void)
{
#ifdef __PKVM_HYP__
	WARN_ON(enable_pmu);
	return 0;
#else
	u64 perf_cap = PMU_CAP_FW_WRITES;
	u64 host_perf_cap = 0;

	if (!enable_pmu)
		return 0;

	if (boot_cpu_has(X86_FEATURE_PDCM))
		rdmsrl(MSR_IA32_PERF_CAPABILITIES, host_perf_cap);

	if (!cpu_feature_enabled(X86_FEATURE_ARCH_LBR)) {
		x86_perf_get_lbr(&vmx_lbr_caps);

		/*
		 * KVM requires LBR callstack support, as the overhead due to
		 * context switching LBRs without said support is too high.
		 * See intel_pmu_create_guest_lbr_event() for more info.
		 */
		if (!vmx_lbr_caps.has_callstack)
			memset(&vmx_lbr_caps, 0, sizeof(vmx_lbr_caps));
		else if (vmx_lbr_caps.nr)
			perf_cap |= host_perf_cap & PMU_CAP_LBR_FMT;
	}

	if (vmx_pebs_supported()) {
		perf_cap |= host_perf_cap & PERF_CAP_PEBS_MASK;

		/*
		 * Disallow adaptive PEBS as it is functionally broken, can be
		 * used by the guest to read *host* LBRs, and can be used to
		 * bypass userspace event filters.  To correctly and safely
		 * support adaptive PEBS, KVM needs to:
		 *
		 * 1. Account for the ADAPTIVE flag when (re)programming fixed
		 *    counters.
		 *
		 * 2. Gain support from perf (or take direct control of counter
		 *    programming) to support events without adaptive PEBS
		 *    enabled for the hardware counter.
		 *
		 * 3. Ensure LBR MSRs cannot hold host data on VM-Entry with
		 *    adaptive PEBS enabled and MSR_PEBS_DATA_CFG.LBRS=1.
		 *
		 * 4. Document which PMU events are effectively exposed to the
		 *    guest via adaptive PEBS, and make adaptive PEBS mutually
		 *    exclusive with KVM_SET_PMU_EVENT_FILTER if necessary.
		 */
		perf_cap &= ~PERF_CAP_PEBS_BASELINE;
	}

	return perf_cap;
#endif
}

static __init void vmx_set_cpu_caps(void)
{
	kvm_set_cpu_caps();

	/* CPUID 0x1 */
	if (nested)
		kvm_cpu_cap_set(X86_FEATURE_VMX);

	/* CPUID 0x7 */
	if (kvm_mpx_supported())
		kvm_cpu_cap_check_and_set(X86_FEATURE_MPX);
	if (!cpu_has_vmx_invpcid())
		kvm_cpu_cap_clear(X86_FEATURE_INVPCID);
	if (vmx_pt_mode_is_host_guest())
		kvm_cpu_cap_check_and_set(X86_FEATURE_INTEL_PT);
#ifndef __PKVM_HYP__
	if (vmx_pebs_supported()) {
		kvm_cpu_cap_check_and_set(X86_FEATURE_DS);
		kvm_cpu_cap_check_and_set(X86_FEATURE_DTES64);
	}
#endif

	if (!enable_pmu)
		kvm_cpu_cap_clear(X86_FEATURE_PDCM);
	kvm_caps.supported_perf_cap = vmx_get_perf_capabilities();

	if (!enable_sgx) {
		kvm_cpu_cap_clear(X86_FEATURE_SGX);
		kvm_cpu_cap_clear(X86_FEATURE_SGX_LC);
		kvm_cpu_cap_clear(X86_FEATURE_SGX1);
		kvm_cpu_cap_clear(X86_FEATURE_SGX2);
		kvm_cpu_cap_clear(X86_FEATURE_SGX_EDECCSSA);
	}

	if (vmx_umip_emulated())
		kvm_cpu_cap_set(X86_FEATURE_UMIP);

	/* CPUID 0xD.1 */
	kvm_caps.supported_xss = 0;
	if (!cpu_has_vmx_xsaves())
		kvm_cpu_cap_clear(X86_FEATURE_XSAVES);

	/* CPUID 0x80000001 and 0x7 (RDPID) */
	if (!cpu_has_vmx_rdtscp()) {
		kvm_cpu_cap_clear(X86_FEATURE_RDTSCP);
		kvm_cpu_cap_clear(X86_FEATURE_RDPID);
	}

	if (cpu_has_vmx_waitpkg())
		kvm_cpu_cap_check_and_set(X86_FEATURE_WAITPKG);
}

static __init void vmx_setup_user_return_msrs(void)
{

	/*
	 * Though SYSCALL is only supported in 64-bit mode on Intel CPUs, kvm
	 * will emulate SYSCALL in legacy mode if the vendor string in guest
	 * CPUID.0:{EBX,ECX,EDX} is "AuthenticAMD" or "AMDisbetter!" To
	 * support this emulation, MSR_STAR is included in the list for i386,
	 * but is never loaded into hardware.  MSR_CSTAR is also never loaded
	 * into hardware and is here purely for emulation purposes.
	 */
	const u32 vmx_uret_msrs_list[] = {
	#ifdef CONFIG_X86_64
		MSR_SYSCALL_MASK, MSR_LSTAR, MSR_CSTAR,
	#endif
		MSR_EFER, MSR_TSC_AUX, MSR_STAR,
		MSR_IA32_TSX_CTRL,
	};
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(vmx_uret_msrs_list) != MAX_NR_USER_RETURN_MSRS);

	for (i = 0; i < ARRAY_SIZE(vmx_uret_msrs_list); ++i) {
		/*
		 * FIXME: As pkvm doesn't implement exception table so cannot
		 * capture the exception generated by accessing MSR. To avoid
		 * this, check cpu capability first before accessing a MSR which
		 * might be not able to support. Once exception table is
		 * supported, this can be removed.
		 */
		if (vmx_uret_msrs_list[i] == MSR_IA32_TSX_CTRL) {
			if (boot_cpu_has(X86_FEATURE_MSR_TSX_CTRL))
				kvm_add_user_return_msr(MSR_IA32_TSX_CTRL);
			continue;
		}

		kvm_add_user_return_msr(vmx_uret_msrs_list[i]);
	}
}

__init int vmx_hardware_setup(void)
{
	unsigned long host_bndcfgs;
	struct desc_ptr dt;
	int r;

	store_idt(&dt);
	host_idt_base = dt.address;

	vmx_setup_user_return_msrs();

	if (setup_vmcs_config(&vmcs_config, &vmx_capability) < 0)
		return -EIO;

	if (cpu_has_perf_global_ctrl_bug())
		pr_warn_once("VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL "
			     "does not work properly. Using workaround\n");

	if (boot_cpu_has(X86_FEATURE_NX))
		kvm_enable_efer_bits(EFER_NX);

	if (boot_cpu_has(X86_FEATURE_MPX)) {
		rdmsrl(MSR_IA32_BNDCFGS, host_bndcfgs);
		WARN_ONCE(host_bndcfgs, "BNDCFGS in host will be lost");
	}

	if (!cpu_has_vmx_mpx())
		kvm_caps.supported_xcr0 &= ~(XFEATURE_MASK_BNDREGS |
					     XFEATURE_MASK_BNDCSR);

	if (!cpu_has_vmx_vpid() || !cpu_has_vmx_invvpid() ||
	    !(cpu_has_vmx_invvpid_single() || cpu_has_vmx_invvpid_global()))
		enable_vpid = 0;

	if (!cpu_has_vmx_ept() ||
	    !cpu_has_vmx_ept_4levels() ||
	    !cpu_has_vmx_ept_mt_wb() ||
	    !cpu_has_vmx_invept_global())
		enable_ept = 0;

	/* NX support is required for shadow paging. */
	if (!enable_ept && !boot_cpu_has(X86_FEATURE_NX)) {
		pr_err_ratelimited("NX (Execute Disable) not supported\n");
		return -EOPNOTSUPP;
	}

	if (!cpu_has_vmx_ept_ad_bits() || !enable_ept)
		enable_ept_ad_bits = 0;

	if (!cpu_has_vmx_unrestricted_guest() || !enable_ept)
		enable_unrestricted_guest = 0;

	if (!cpu_has_vmx_flexpriority())
		flexpriority_enabled = 0;

	if (!cpu_has_virtual_nmis())
		enable_vnmi = 0;

#ifdef CONFIG_X86_SGX_KVM
	if (!cpu_has_vmx_encls_vmexit())
		enable_sgx = false;
#endif

	/*
	 * set_apic_access_page_addr() is used to reload apic access
	 * page upon invalidation.  No need to do anything if not
	 * using the APIC_ACCESS_ADDR VMCS field.
	 */
	if (!flexpriority_enabled)
		vt_x86_ops.set_apic_access_page_addr = NULL;

	if (!cpu_has_vmx_tpr_shadow())
		vt_x86_ops.update_cr8_intercept = NULL;

#if IS_ENABLED(CONFIG_HYPERV) && !defined __PKVM_HYP__
	if (ms_hyperv.nested_features & HV_X64_NESTED_GUEST_MAPPING_FLUSH
	    && enable_ept) {
		vt_x86_ops.flush_remote_tlbs = hv_flush_remote_tlbs;
		vt_x86_ops.flush_remote_tlbs_range = hv_flush_remote_tlbs_range;
	}
#endif

	if (!cpu_has_vmx_ple()) {
		ple_gap = 0;
		ple_window = 0;
		ple_window_grow = 0;
		ple_window_max = 0;
		ple_window_shrink = 0;
	}

	if (!cpu_has_vmx_apicv())
		enable_apicv = 0;
	if (!enable_apicv)
		vt_x86_ops.sync_pir_to_irr = NULL;

	if (!enable_apicv || !cpu_has_vmx_ipiv())
		enable_ipiv = false;

	if (cpu_has_vmx_tsc_scaling())
		kvm_caps.has_tsc_control = true;

	kvm_caps.max_tsc_scaling_ratio = KVM_VMX_TSC_MULTIPLIER_MAX;
	kvm_caps.tsc_scaling_ratio_frac_bits = 48;
	kvm_caps.has_bus_lock_exit = cpu_has_vmx_bus_lock_detection();
	kvm_caps.has_notify_vmexit = cpu_has_notify_vmexit();

	set_bit(0, vmx_vpid_bitmap); /* 0 is reserved for host */

#ifndef __PKVM_HYP__ /* FIXME: Coordinate with PV EPT */
	if (enable_ept)
		kvm_mmu_set_ept_masks(enable_ept_ad_bits,
				      cpu_has_vmx_ept_execute_only());

	/*
	 * Setup shadow_me_value/shadow_me_mask to include MKTME KeyID
	 * bits to shadow_zero_check.
	 */
	vmx_setup_me_spte_mask();

	kvm_configure_mmu(enable_ept, 0, vmx_get_max_ept_level(),
			  ept_caps_to_lpage_level(vmx_capability.ept));
#endif

	/*
	 * Only enable PML when hardware supports PML feature, and both EPT
	 * and EPT A/D bit features are enabled -- PML depends on them to work.
	 */
	if (!enable_ept || !enable_ept_ad_bits || !cpu_has_vmx_pml())
		enable_pml = 0;

	if (!enable_pml)
		vt_x86_ops.cpu_dirty_log_size = 0;

	if (!cpu_has_vmx_preemption_timer())
		enable_preemption_timer = false;

	if (enable_preemption_timer) {
		u64 use_timer_freq = 5000ULL * 1000 * 1000;

		cpu_preemption_timer_multi =
			vmx_misc_preemption_timer_rate(vmcs_config.misc);

		if (tsc_khz)
			use_timer_freq = (u64)tsc_khz * 1000;
		use_timer_freq >>= cpu_preemption_timer_multi;

		/*
		 * KVM "disables" the preemption timer by setting it to its max
		 * value.  Don't use the timer if it might cause spurious exits
		 * at a rate faster than 0.1 Hz (of uninterrupted guest time).
		 */
		if (use_timer_freq > 0xffffffffu / 10)
			enable_preemption_timer = false;
	}

	if (!enable_preemption_timer) {
		vt_x86_ops.set_hv_timer = NULL;
		vt_x86_ops.cancel_hv_timer = NULL;
	}

	kvm_caps.supported_mce_cap |= MCG_LMCE_P;
	kvm_caps.supported_mce_cap |= MCG_CMCI_P;

	if (pt_mode != PT_MODE_SYSTEM && pt_mode != PT_MODE_HOST_GUEST)
		return -EINVAL;
	if (!enable_ept || !enable_pmu || !cpu_has_vmx_intel_pt())
		pt_mode = PT_MODE_SYSTEM;
	if (pt_mode == PT_MODE_HOST_GUEST)
#ifdef __PKVM_HYP__
		return -EINVAL;
#else
		vt_init_ops.handle_intel_pt_intr = vmx_handle_intel_pt_intr;
#endif
	else
		vt_init_ops.handle_intel_pt_intr = NULL;

	setup_default_sgx_lepubkeyhash();

	if (nested) {
		nested_vmx_setup_ctls_msrs(&vmcs_config, vmx_capability.ept);

		r = nested_vmx_hardware_setup(kvm_vmx_exit_handlers);
		if (r)
			return r;
	}

	vmx_set_cpu_caps();

	r = alloc_kvm_area();
	if (r && nested)
		nested_vmx_hardware_unsetup();

#ifndef __PKVM_HYP__
	kvm_set_posted_intr_wakeup_handler(pi_wakeup_handler);
#endif

	return r;
}

#ifdef __PKVM_HYP__
struct kvm_x86_ops vt_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.enable_virtualization_cpu = vmx_enable_virtualization_cpu,
	.disable_virtualization_cpu = vmx_disable_virtualization_cpu,
};

struct kvm_x86_init_ops vt_init_ops __initdata = {
	.hardware_setup = vmx_hardware_setup,
	.runtime_ops = &vt_x86_ops,
};

int setup_vmx(void)
{
	int cpu;

	/*
	 * FIXME: No pmu emulation in the pkvm hypervisor to simplify the POC.
	 * Revisit later to see if it is possible to enable PMU support.
	 *
	 * TODO: Add PMU isolation, to prevent the host from profiling the
	 * guest.
	 */
	enable_pmu = false;

	/* No VMX emulation in the pkvm hypervisor */
	nested = false;

#ifdef CONFIG_X86_SGX_KVM
	/*
	 * FIXME: No SGX emulation in the pkvm hypervisor to simplify the POC.
	 * Revisit later to see if it is possible to enable the SGX.
	 */
	enable_sgx = false;
#endif

	/*
	 * FIXME: the pkvm hypervisor emulated MSR_IA32_VMX_EPT_VPID_CAP by
	 * removing VMX_EPT_AD_BIT. So the host KVM cannot see this bit. To
	 * keep align with the host KVM, disable enable_ept_ad_bits in the pkvm
	 * hypervisor. Revisit later when PV method is fully functional.
	 */
	enable_ept_ad_bits = 0;

	for_each_possible_cpu(cpu)
		INIT_LIST_HEAD(&per_cpu(loaded_vmcss_on_cpu, cpu));

	return kvm_x86_vendor_init(&vt_init_ops);
}
#endif
