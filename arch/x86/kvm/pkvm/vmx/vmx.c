// SPDX-License-Identifier: GPL-2.0
#include <linux/array_size.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <vmx/vmx_ops.h>
#include <asm/cpuid.h>
#include <asm/msr.h>
#include <asm/debugreg.h>
#include <vmx/vmx.h>
#include <vmx/x86_ops.h>
#include <vmx/nested.h>
#include <vmx/sgx.h>
#include "vmx.h"
#include <trace.h>
#include <pkvm/pkvm.h>
#include <vmx/pkvm/hyp/pkvm_hyp.h>
#include <vmx/pkvm/hyp/mem_protect.h>
#include <vmx/pkvm/hyp/nested.h>
#include <vmx/pkvm/hyp/ept.h>
#include <vmx/hyperv.h>
#include <pkvm.h>

#ifdef __PKVM_HYP__
#undef module_param_named
#define module_param_named(...)
#endif

bool __read_mostly enable_vpid = 1;
module_param_named(vpid, enable_vpid, bool, 0444);

bool __read_mostly enable_vnmi = 1;
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

static bool __read_mostly dump_invalid_vmcs = 0;
module_param(dump_invalid_vmcs, bool, 0644);

static bool __read_mostly error_on_inconsistent_vmcs_config = true;
module_param(error_on_inconsistent_vmcs_config, bool, 0444);

#define KVM_VMX_TSC_MULTIPLIER_MAX     0xffffffffffffffffULL

/* Guest_tsc -> host_tsc conversion requires 64-bit division.  */
static int __read_mostly cpu_preemption_timer_multi;
static bool __read_mostly enable_preemption_timer = 1;
#ifdef CONFIG_X86_64
module_param_named(preemption_timer, enable_preemption_timer, bool, S_IRUGO);
#endif

extern bool __read_mostly allow_smaller_maxphyaddr;
module_param(allow_smaller_maxphyaddr, bool, S_IRUGO);

#define RMODE_GUEST_OWNED_EFLAGS_BITS (~(X86_EFLAGS_IOPL | X86_EFLAGS_VM))

/*
 * List of MSRs that can be directly passed to the guest.
 * In addition to these x2apic, PT and LBR MSRs are handled specially.
 */
static u32 vmx_possible_passthrough_msrs[MAX_POSSIBLE_PASSTHROUGH_MSRS] = {
	MSR_IA32_SPEC_CTRL,
	MSR_IA32_PRED_CMD,
	MSR_IA32_FLUSH_CMD,
	MSR_IA32_TSC,
#ifdef CONFIG_X86_64
	MSR_FS_BASE,
	MSR_GS_BASE,
	MSR_KERNEL_GS_BASE,
	MSR_IA32_XFD,
	MSR_IA32_XFD_ERR,
#endif
	MSR_IA32_SYSENTER_CS,
	MSR_IA32_SYSENTER_ESP,
	MSR_IA32_SYSENTER_EIP,
	MSR_CORE_C1_RES,
	MSR_CORE_C3_RESIDENCY,
	MSR_CORE_C6_RESIDENCY,
	MSR_CORE_C7_RESIDENCY,
};

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

static __always_inline void vmx_disable_fb_clear(struct vcpu_vmx *vmx)
{
	u64 msr;

	if (!vmx->disable_fb_clear)
		return;

	msr = __rdmsr(MSR_IA32_MCU_OPT_CTRL);
	msr |= FB_CLEAR_DIS;
	native_wrmsrl(MSR_IA32_MCU_OPT_CTRL, msr);
	/* Cache the MSR value to avoid reading it later */
	vmx->msr_ia32_mcu_opt_ctrl = msr;
}

static __always_inline void vmx_enable_fb_clear(struct vcpu_vmx *vmx)
{
	if (!vmx->disable_fb_clear)
		return;

	vmx->msr_ia32_mcu_opt_ctrl &= ~FB_CLEAR_DIS;
	native_wrmsrl(MSR_IA32_MCU_OPT_CTRL, vmx->msr_ia32_mcu_opt_ctrl);
}

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

noinline void vmptrst_error(void)
{
	vmx_insn_failed("vmptrst failed\n");
}

#ifndef CONFIG_CC_HAS_ASM_GOTO_OUTPUT
noinstr void vmptrst_error_trampoline2(bool fault)
{
	if (fault) {
		kvm_spurious_fault();
	} else {
		instrumentation_begin();
		vmptrst_error();
		instrumentation_end();
	}
}
#endif

DEFINE_PER_CPU(struct vmcs *, current_vmcs);
/*
 * We maintain a per-CPU linked-list of VMCS loaded on that CPU. This is needed
 * when a CPU is brought down, and we need to VMCLEAR all VMCSs loaded on it.
 */
static DEFINE_PER_CPU(struct list_head, loaded_vmcss_on_cpu);

static DECLARE_BITMAP(vmx_vpid_bitmap, VMX_NR_VPIDS);
#ifdef __PKVM_HYP__
static pkvm_spinlock_t vmx_vpid_lock;
#else
static DEFINE_SPINLOCK(vmx_vpid_lock);
#endif

struct vmcs_config vmcs_config __ro_after_init;
struct vmx_capability vmx_capability __ro_after_init;

#define VMX_SEGMENT_FIELD(seg)					\
	[VCPU_SREG_##seg] = {                                   \
		.selector = GUEST_##seg##_SELECTOR,		\
		.base = GUEST_##seg##_BASE,		   	\
		.limit = GUEST_##seg##_LIMIT,		   	\
		.ar_bytes = GUEST_##seg##_AR_BYTES,	   	\
	}

static const struct kvm_vmx_segment_field {
	unsigned selector;
	unsigned base;
	unsigned limit;
	unsigned ar_bytes;
} kvm_vmx_segment_fields[] = {
	VMX_SEGMENT_FIELD(CS),
	VMX_SEGMENT_FIELD(DS),
	VMX_SEGMENT_FIELD(ES),
	VMX_SEGMENT_FIELD(FS),
	VMX_SEGMENT_FIELD(GS),
	VMX_SEGMENT_FIELD(SS),
	VMX_SEGMENT_FIELD(TR),
	VMX_SEGMENT_FIELD(LDTR),
};

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

static int vmx_get_passthrough_msr_slot(u32 msr)
{
	int i;

	switch (msr) {
	case 0x800 ... 0x8ff:
		/* x2APIC MSRs. These are handled in vmx_update_msr_bitmap_x2apic() */
		return -ENOENT;
	case MSR_IA32_RTIT_STATUS:
	case MSR_IA32_RTIT_OUTPUT_BASE:
	case MSR_IA32_RTIT_OUTPUT_MASK:
	case MSR_IA32_RTIT_CR3_MATCH:
	case MSR_IA32_RTIT_ADDR0_A ... MSR_IA32_RTIT_ADDR3_B:
		/* PT MSRs. These are handled in pt_update_intercept_for_msr() */
	case MSR_LBR_SELECT:
	case MSR_LBR_TOS:
	case MSR_LBR_INFO_0 ... MSR_LBR_INFO_0 + 31:
	case MSR_LBR_NHM_FROM ... MSR_LBR_NHM_FROM + 31:
	case MSR_LBR_NHM_TO ... MSR_LBR_NHM_TO + 31:
	case MSR_LBR_CORE_FROM ... MSR_LBR_CORE_FROM + 8:
	case MSR_LBR_CORE_TO ... MSR_LBR_CORE_TO + 8:
		/* LBR MSRs. These are handled in vmx_update_intercept_for_lbr_msrs() */
		return -ENOENT;
	}

	for (i = 0; i < ARRAY_SIZE(vmx_possible_passthrough_msrs); i++) {
		if (vmx_possible_passthrough_msrs[i] == msr)
			return i;
	}

	WARN(1, "Invalid MSR %x, please adapt vmx_possible_passthrough_msrs[]", msr);
	return -ENOENT;
}

struct vmx_uret_msr *vmx_find_uret_msr(struct vcpu_vmx *vmx, u32 msr)
{
	int i;

	i = kvm_find_user_return_msr(msr);
	if (i >= 0)
		return &vmx->guest_uret_msrs[i];
	return NULL;
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

#ifndef __PKVM_HYP__
void loaded_vmcs_clear(struct loaded_vmcs *loaded_vmcs)
{
	int cpu = loaded_vmcs->cpu;

	if (cpu != -1)
		smp_call_function_single(cpu,
			 __loaded_vmcs_clear, loaded_vmcs, 1);
}
#endif

static bool vmx_segment_cache_test_set(struct vcpu_vmx *vmx, unsigned seg,
				       unsigned field)
{
	bool ret;
	u32 mask = 1 << (seg * SEG_FIELD_NR + field);

	if (!kvm_register_is_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS)) {
		kvm_register_mark_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS);
		vmx->segment_cache.bitmask = 0;
	}
	ret = vmx->segment_cache.bitmask & mask;
	vmx->segment_cache.bitmask |= mask;
	return ret;
}

static u32 vmx_read_guest_seg_ar(struct vcpu_vmx *vmx, unsigned seg)
{
	u32 *p = &vmx->segment_cache.seg[seg].ar;

	if (!vmx_segment_cache_test_set(vmx, seg, SEG_FIELD_AR))
		*p = vmcs_read32(kvm_vmx_segment_fields[seg].ar_bytes);
	return *p;
}

/*
 * Check if MSR is intercepted for currently loaded MSR bitmap.
 */
static bool msr_write_intercepted(struct vcpu_vmx *vmx, u32 msr)
{
	if (!(exec_controls_get(vmx) & CPU_BASED_USE_MSR_BITMAPS))
		return true;

	return vmx_test_msr_bitmap_write(vmx->loaded_vmcs->msr_bitmap, msr);
}

unsigned int __vmx_vcpu_run_flags(struct vcpu_vmx *vmx)
{
	unsigned int flags = 0;

	if (vmx->loaded_vmcs->launched)
		flags |= VMX_RUN_VMRESUME;

	/*
	 * If writes to the SPEC_CTRL MSR aren't intercepted, the guest is free
	 * to change it directly without causing a vmexit.  In that case read
	 * it after vmexit and store it in vmx->spec_ctrl.
	 */
	if (!msr_write_intercepted(vmx, MSR_IA32_SPEC_CTRL))
		flags |= VMX_RUN_SAVE_SPEC_CTRL;

	return flags;
}

static __always_inline void clear_atomic_switch_msr_special(struct vcpu_vmx *vmx,
		unsigned long entry, unsigned long exit)
{
	vm_entry_controls_clearbit(vmx, entry);
	vm_exit_controls_clearbit(vmx, exit);
}

int vmx_find_loadstore_msr_slot(struct vmx_msrs *m, u32 msr)
{
	unsigned int i;

	for (i = 0; i < m->nr; ++i) {
		if (m->val[i].index == msr)
			return i;
	}
	return -ENOENT;
}

static void clear_atomic_switch_msr(struct vcpu_vmx *vmx, unsigned msr)
{
	int i;
	struct msr_autoload *m = &vmx->msr_autoload;

	switch (msr) {
	case MSR_EFER:
		if (cpu_has_load_ia32_efer()) {
			clear_atomic_switch_msr_special(vmx,
					VM_ENTRY_LOAD_IA32_EFER,
					VM_EXIT_LOAD_IA32_EFER);
			return;
		}
		break;
	case MSR_CORE_PERF_GLOBAL_CTRL:
		if (cpu_has_load_perf_global_ctrl()) {
			clear_atomic_switch_msr_special(vmx,
					VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL,
					VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL);
			return;
		}
		break;
	}
	i = vmx_find_loadstore_msr_slot(&m->guest, msr);
	if (i < 0)
		goto skip_guest;
	--m->guest.nr;
	m->guest.val[i] = m->guest.val[m->guest.nr];
	vmcs_write32(VM_ENTRY_MSR_LOAD_COUNT, m->guest.nr);

skip_guest:
	i = vmx_find_loadstore_msr_slot(&m->host, msr);
	if (i < 0)
		return;

	--m->host.nr;
	m->host.val[i] = m->host.val[m->host.nr];
	vmcs_write32(VM_EXIT_MSR_LOAD_COUNT, m->host.nr);
}

static __always_inline void add_atomic_switch_msr_special(struct vcpu_vmx *vmx,
		unsigned long entry, unsigned long exit,
		unsigned long guest_val_vmcs, unsigned long host_val_vmcs,
		u64 guest_val, u64 host_val)
{
	vmcs_write64(guest_val_vmcs, guest_val);
	if (host_val_vmcs != HOST_IA32_EFER)
		vmcs_write64(host_val_vmcs, host_val);
	vm_entry_controls_setbit(vmx, entry);
	vm_exit_controls_setbit(vmx, exit);
}

static void add_atomic_switch_msr(struct vcpu_vmx *vmx, unsigned msr,
				  u64 guest_val, u64 host_val, bool entry_only)
{
	int i, j = 0;
	struct msr_autoload *m = &vmx->msr_autoload;

	switch (msr) {
	case MSR_EFER:
		if (cpu_has_load_ia32_efer()) {
			add_atomic_switch_msr_special(vmx,
					VM_ENTRY_LOAD_IA32_EFER,
					VM_EXIT_LOAD_IA32_EFER,
					GUEST_IA32_EFER,
					HOST_IA32_EFER,
					guest_val, host_val);
			return;
		}
		break;
	case MSR_CORE_PERF_GLOBAL_CTRL:
		if (cpu_has_load_perf_global_ctrl()) {
			add_atomic_switch_msr_special(vmx,
					VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL,
					VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL,
					GUEST_IA32_PERF_GLOBAL_CTRL,
					HOST_IA32_PERF_GLOBAL_CTRL,
					guest_val, host_val);
			return;
		}
		break;
	case MSR_IA32_PEBS_ENABLE:
		/* PEBS needs a quiescent period after being disabled (to write
		 * a record).  Disabling PEBS through VMX MSR swapping doesn't
		 * provide that period, so a CPU could write host's record into
		 * guest's memory.
		 */
		wrmsrl(MSR_IA32_PEBS_ENABLE, 0);
	}

	i = vmx_find_loadstore_msr_slot(&m->guest, msr);
	if (!entry_only)
		j = vmx_find_loadstore_msr_slot(&m->host, msr);

	if ((i < 0 && m->guest.nr == MAX_NR_LOADSTORE_MSRS) ||
	    (j < 0 &&  m->host.nr == MAX_NR_LOADSTORE_MSRS)) {
		printk_once(KERN_WARNING "Not enough msr switch entries. "
				"Can't add msr %x\n", msr);
		return;
	}
	if (i < 0) {
		i = m->guest.nr++;
		vmcs_write32(VM_ENTRY_MSR_LOAD_COUNT, m->guest.nr);
	}
	m->guest.val[i].index = msr;
	m->guest.val[i].value = guest_val;

	if (entry_only)
		return;

	if (j < 0) {
		j = m->host.nr++;
		vmcs_write32(VM_EXIT_MSR_LOAD_COUNT, m->host.nr);
	}
	m->host.val[j].index = msr;
	m->host.val[j].value = host_val;
}

static inline void pt_load_msr(struct pt_ctx *ctx, u32 addr_range)
{
	u32 i;

	wrmsrl(MSR_IA32_RTIT_STATUS, ctx->status);
	wrmsrl(MSR_IA32_RTIT_OUTPUT_BASE, ctx->output_base);
	wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK, ctx->output_mask);
	wrmsrl(MSR_IA32_RTIT_CR3_MATCH, ctx->cr3_match);
	for (i = 0; i < addr_range; i++) {
		wrmsrl(MSR_IA32_RTIT_ADDR0_A + i * 2, ctx->addr_a[i]);
		wrmsrl(MSR_IA32_RTIT_ADDR0_B + i * 2, ctx->addr_b[i]);
	}
}

static inline void pt_save_msr(struct pt_ctx *ctx, u32 addr_range)
{
	u32 i;

	rdmsrl(MSR_IA32_RTIT_STATUS, ctx->status);
	rdmsrl(MSR_IA32_RTIT_OUTPUT_BASE, ctx->output_base);
	rdmsrl(MSR_IA32_RTIT_OUTPUT_MASK, ctx->output_mask);
	rdmsrl(MSR_IA32_RTIT_CR3_MATCH, ctx->cr3_match);
	for (i = 0; i < addr_range; i++) {
		rdmsrl(MSR_IA32_RTIT_ADDR0_A + i * 2, ctx->addr_a[i]);
		rdmsrl(MSR_IA32_RTIT_ADDR0_B + i * 2, ctx->addr_b[i]);
	}
}

static void pt_guest_enter(struct vcpu_vmx *vmx)
{
	if (vmx_pt_mode_is_system())
		return;

	/*
	 * GUEST_IA32_RTIT_CTL is already set in the VMCS.
	 * Save host state before VM entry.
	 */
	rdmsrl(MSR_IA32_RTIT_CTL, vmx->pt_desc.host.ctl);
	if (vmx->pt_desc.guest.ctl & RTIT_CTL_TRACEEN) {
		wrmsrl(MSR_IA32_RTIT_CTL, 0);
		pt_save_msr(&vmx->pt_desc.host, vmx->pt_desc.num_address_ranges);
		pt_load_msr(&vmx->pt_desc.guest, vmx->pt_desc.num_address_ranges);
	}
}

static void pt_guest_exit(struct vcpu_vmx *vmx)
{
	if (vmx_pt_mode_is_system())
		return;

	if (vmx->pt_desc.guest.ctl & RTIT_CTL_TRACEEN) {
		pt_save_msr(&vmx->pt_desc.guest, vmx->pt_desc.num_address_ranges);
		pt_load_msr(&vmx->pt_desc.host, vmx->pt_desc.num_address_ranges);
	}

	/*
	 * KVM requires VM_EXIT_CLEAR_IA32_RTIT_CTL to expose PT to the guest,
	 * i.e. RTIT_CTL is always cleared on VM-Exit.  Restore it if necessary.
	 */
	if (vmx->pt_desc.host.ctl)
		wrmsrl(MSR_IA32_RTIT_CTL, vmx->pt_desc.host.ctl);
}

void vmx_set_host_fs_gs(struct vmcs_host_state *host, u16 fs_sel, u16 gs_sel,
			unsigned long fs_base, unsigned long gs_base)
{
	if (unlikely(fs_sel != host->fs_sel)) {
		if (!(fs_sel & 7))
			vmcs_write16(HOST_FS_SELECTOR, fs_sel);
		else
			vmcs_write16(HOST_FS_SELECTOR, 0);
		host->fs_sel = fs_sel;
	}
	if (unlikely(gs_sel != host->gs_sel)) {
		if (!(gs_sel & 7))
			vmcs_write16(HOST_GS_SELECTOR, gs_sel);
		else
			vmcs_write16(HOST_GS_SELECTOR, 0);
		host->gs_sel = gs_sel;
	}
	if (unlikely(fs_base != host->fs_base)) {
		vmcs_writel(HOST_FS_BASE, fs_base);
		host->fs_base = fs_base;
	}
	if (unlikely(gs_base != host->gs_base)) {
		vmcs_writel(HOST_GS_BASE, gs_base);
		host->gs_base = gs_base;
	}
}

void vmx_prepare_switch_to_guest(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct vmcs_host_state *host_state;
	unsigned long fs_base, gs_base;
	u16 fs_sel, gs_sel;
#ifndef __PKVM_HYP__ /* TODO: set uret MSR */
	int i;

	/*
	 * Note that guest MSRs to be saved/restored can also be changed
	 * when guest state is loaded. This happens when guest transitions
	 * to/from long-mode by setting MSR_EFER.LMA.
	 */
	if (!vmx->guest_uret_msrs_loaded) {
		vmx->guest_uret_msrs_loaded = true;
		for (i = 0; i < kvm_nr_uret_msrs; ++i) {
			if (!vmx->guest_uret_msrs[i].load_into_hardware)
				continue;

			kvm_set_user_return_msr(i,
						vmx->guest_uret_msrs[i].data,
						vmx->guest_uret_msrs[i].mask);
		}
	}
#endif

	if (vmx->nested.need_vmcs12_to_shadow_sync)
		nested_sync_vmcs12_to_shadow(vcpu);

	if (vmx->guest_state_loaded)
		return;

	host_state = &vmx->loaded_vmcs->host_state;

	/*
	 * Set host fs and gs selectors.  Unfortunately, 22.2.3 does not
	 * allow segment selectors with cpl > 0 or ti == 1.
	 */
	host_state->ldt_sel = kvm_read_ldt();

#ifdef __PKVM_HYP__

#ifdef CONFIG_X86_64
	savesegment(ds, host_state->ds_sel);
	savesegment(es, host_state->es_sel);
	savesegment(fs, fs_sel);
	savesegment(gs, gs_sel);
	fs_base = read_msr(MSR_FS_BASE);
	gs_base = read_msr(MSR_GS_BASE);
	vmx->msr_host_kernel_gs_base = read_msr(MSR_KERNEL_GS_BASE);
	wrmsrl(MSR_KERNEL_GS_BASE, vmx->msr_guest_kernel_gs_base);
#else
	/* TODO */
	WARN_ON_ONCE(1);
#endif
#else

#ifdef CONFIG_X86_64
	savesegment(ds, host_state->ds_sel);
	savesegment(es, host_state->es_sel);

	gs_base = cpu_kernelmode_gs_base(raw_smp_processor_id());
	if (likely(is_64bit_mm(current->mm))) {
		current_save_fsgs();
		fs_sel = current->thread.fsindex;
		gs_sel = current->thread.gsindex;
		fs_base = current->thread.fsbase;
		vmx->msr_host_kernel_gs_base = current->thread.gsbase;
	} else {
		savesegment(fs, fs_sel);
		savesegment(gs, gs_sel);
		fs_base = read_msr(MSR_FS_BASE);
		vmx->msr_host_kernel_gs_base = read_msr(MSR_KERNEL_GS_BASE);
	}
#else
	savesegment(fs, fs_sel);
	savesegment(gs, gs_sel);
	fs_base = segment_base(fs_sel);
	gs_base = segment_base(gs_sel);
#endif
#endif
	vmx_set_host_fs_gs(host_state, fs_sel, gs_sel, fs_base, gs_base);
	vmx->guest_state_loaded = true;
}

static void vmx_prepare_switch_to_host(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct vmcs_host_state *host_state;

	if (!vmx->guest_state_loaded)
		return;

	host_state = &vmx->loaded_vmcs->host_state;

	++vmx->vcpu.stat.host_state_reload;

#ifdef CONFIG_X86_64
	rdmsrl(MSR_KERNEL_GS_BASE, vmx->msr_guest_kernel_gs_base);
#endif

#ifdef __PKVM_HYP__
#ifdef CONFIG_X86_64
	if (unlikely(host_state->ds_sel | host_state->es_sel)) {
		loadsegment(ds, host_state->ds_sel);
		loadsegment(es, host_state->es_sel);
	}
	wrmsrl(MSR_KERNEL_GS_BASE, vmx->msr_host_kernel_gs_base);
#endif
#else
	if (host_state->ldt_sel || (host_state->gs_sel & 7)) {
		kvm_load_ldt(host_state->ldt_sel);
#ifdef CONFIG_X86_64
		load_gs_index(host_state->gs_sel);
#else
		loadsegment(gs, host_state->gs_sel);
#endif
	}
	if (host_state->fs_sel & 7)
		loadsegment(fs, host_state->fs_sel);
#ifdef CONFIG_X86_64
	if (unlikely(host_state->ds_sel | host_state->es_sel)) {
		loadsegment(ds, host_state->ds_sel);
		loadsegment(es, host_state->es_sel);
	}
#endif
	invalidate_tss_limit();
#ifdef CONFIG_X86_64
	wrmsrl(MSR_KERNEL_GS_BASE, vmx->msr_host_kernel_gs_base);
#endif
	load_fixmap_gdt(raw_smp_processor_id());
#endif

	vmx->guest_state_loaded = false;
#ifndef __PKVM_HYP__
	vmx->guest_uret_msrs_loaded = false;
#endif
}

static void shrink_ple_window(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned int old = vmx->ple_window;

	vmx->ple_window = __shrink_ple_window(old, ple_window,
					      ple_window_shrink,
					      ple_window);

	if (vmx->ple_window != old) {
		vmx->ple_window_dirty = true;
		trace_kvm_ple_window_update(vcpu->vcpu_id,
					    vmx->ple_window, old);
	}
}

#ifdef __PKVM_HYP__
static unsigned long get_pkvm_tss(int cpu)
{
#ifdef CONFIG_PKVM_INTEL_DEBUG
	return (unsigned long)&get_cpu_entry_area(cpu)->tss.x86_tss;
#else
	struct pkvm_host_vcpu *hvcpu = to_pkvm_hvcpu(per_cpu(host_vcpu, cpu));

	return (unsigned long)&hvcpu->pcpu->tss;
#endif
}
#endif

void vmx_vcpu_load_vmcs(struct kvm_vcpu *vcpu, int cpu,
			struct loaded_vmcs *buddy)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	bool already_loaded = vmx->loaded_vmcs->cpu == cpu;
	struct vmcs *prev;

	if (!already_loaded) {
#ifdef __PKVM_HYP__
		/*
		 * pkvm doesn't support smp call thus doesn't support clear vmcs
		 * on a remote CPU. Suppose this vmcs is already cleared by
		 * vmx_vcpu_put, otherwise it cannot be loaded on this CPU.
		 */
		if (WARN_ON_ONCE(vmx->loaded_vmcs->cpu != -1))
			return;
#else
		loaded_vmcs_clear(vmx->loaded_vmcs);
#endif
		local_irq_disable();

		/*
		 * Ensure loaded_vmcs->cpu is read before adding loaded_vmcs to
		 * this cpu's percpu list, otherwise it may not yet be deleted
		 * from its previous cpu's percpu list.  Pairs with the
		 * smb_wmb() in __loaded_vmcs_clear().
		 */
		smp_rmb();

		list_add(&vmx->loaded_vmcs->loaded_vmcss_on_cpu_link,
			 &per_cpu(loaded_vmcss_on_cpu, cpu));
		local_irq_enable();
	}

	prev = per_cpu(current_vmcs, cpu);
	if (prev != vmx->loaded_vmcs->vmcs) {
		per_cpu(current_vmcs, cpu) = vmx->loaded_vmcs->vmcs;
		vmcs_load(vmx->loaded_vmcs->vmcs);
		/*
		 * No indirect branch prediction barrier needed when switching
		 * the active VMCS within a vCPU, unless IBRS is advertised to
		 * the vCPU.  To minimize the number of IBPBs executed, KVM
		 * performs IBPB on nested VM-Exit (a single nested transition
		 * may switch the active VMCS multiple times).
		 */
		if (!buddy || WARN_ON_ONCE(buddy->vmcs != prev))
			indirect_branch_prediction_barrier();
	}

	if (!already_loaded) {
#ifdef __PKVM_HYP__
		struct desc_ptr gdt;
		/*
		 * Flush all EPTP/VPID contexts, the new pCPU may have stale
		 * TLB entries from its previous association with the vCPU.
		 */
		kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu);

		vmcs_writel(HOST_TR_BASE, get_pkvm_tss(cpu));

		native_store_gdt(&gdt);
		vmcs_writel(HOST_GDTR_BASE, gdt.address);

		if (IS_ENABLED(CONFIG_IA32_EMULATION) || IS_ENABLED(CONFIG_X86_32)) {
			unsigned long msr = __rdmsr(MSR_IA32_SYSENTER_ESP);

			vmcs_writel(HOST_IA32_SYSENTER_ESP, msr);
		}
#else
		void *gdt = get_current_gdt_ro();

		/*
		 * Flush all EPTP/VPID contexts, the new pCPU may have stale
		 * TLB entries from its previous association with the vCPU.
		 */
		kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu);

		/*
		 * Linux uses per-cpu TSS and GDT, so set these when switching
		 * processors.  See 22.2.4.
		 */
		vmcs_writel(HOST_TR_BASE,
			    (unsigned long)&get_cpu_entry_area(cpu)->tss.x86_tss);
		vmcs_writel(HOST_GDTR_BASE, (unsigned long)gdt);   /* 22.2.4 */

		if (IS_ENABLED(CONFIG_IA32_EMULATION) || IS_ENABLED(CONFIG_X86_32)) {
			/* 22.2.3 */
			vmcs_writel(HOST_IA32_SYSENTER_ESP,
				    (unsigned long)(cpu_entry_stack(cpu) + 1));
		}
#endif
		vmx->loaded_vmcs->cpu = cpu;
	}
}

/*
 * Switches to specified vcpu, until a matching vcpu_put(), but assumes
 * vcpu mutex is already taken.
 */
void vmx_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	if (vcpu->scheduled_out && !kvm_pause_in_guest(vcpu->kvm))
		shrink_ple_window(vcpu);

	vmx_vcpu_load_vmcs(vcpu, cpu, NULL);

#ifndef __PKVM_HYP__
	vmx_vcpu_pi_load(vcpu, cpu);
#endif

#ifdef __PKVM_HYP__
	/*
	 * As the VMX emulate-based method is still in use, load the vmcs
	 * following the nested way.
	 */
	pkvm_nested_load_vmcs(vcpu);
#endif
}

void vmx_vcpu_put(struct kvm_vcpu *vcpu)
{
#ifdef __PKVM_HYP__
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int cpu = raw_smp_processor_id();

	if (vmx->loaded_vmcs->cpu == -1 ||
			WARN_ON_ONCE(vmx->loaded_vmcs->cpu != cpu))
		return;

	/* In pair with pkvm_nested_load_vmcs02 in vcpu_load. */
	pkvm_nested_release_vmcs(vcpu);
	__loaded_vmcs_clear(vmx->loaded_vmcs);
#else
	vmx_vcpu_pi_put(vcpu);

	vmx_prepare_switch_to_host(to_vmx(vcpu));
#endif
}

unsigned long vmx_get_rflags(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long rflags, save_rflags;

	if (!kvm_register_is_available(vcpu, VCPU_EXREG_RFLAGS)) {
		kvm_register_mark_available(vcpu, VCPU_EXREG_RFLAGS);
		rflags = vmcs_readl(GUEST_RFLAGS);
		if (vmx->rmode.vm86_active) {
			rflags &= RMODE_GUEST_OWNED_EFLAGS_BITS;
			save_rflags = vmx->rmode.save_rflags;
			rflags |= save_rflags & ~RMODE_GUEST_OWNED_EFLAGS_BITS;
		}
		vmx->rflags = rflags;
	}
	return vmx->rflags;
}

void vmx_set_interrupt_shadow(struct kvm_vcpu *vcpu, int mask)
{
	u32 interruptibility_old = vmcs_read32(GUEST_INTERRUPTIBILITY_INFO);
	u32 interruptibility = interruptibility_old;

	interruptibility &= ~(GUEST_INTR_STATE_STI | GUEST_INTR_STATE_MOV_SS);

	if (mask & KVM_X86_SHADOW_INT_MOV_SS)
		interruptibility |= GUEST_INTR_STATE_MOV_SS;
	else if (mask & KVM_X86_SHADOW_INT_STI)
		interruptibility |= GUEST_INTR_STATE_STI;

	if ((interruptibility != interruptibility_old))
		vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, interruptibility);
}

static int skip_emulated_instruction(struct kvm_vcpu *vcpu)
{
	union vmx_exit_reason exit_reason = to_vmx(vcpu)->exit_reason;
	unsigned long rip, orig_rip;
	u32 instr_len;

	/*
	 * Using VMCS.VM_EXIT_INSTRUCTION_LEN on EPT misconfig depends on
	 * undefined behavior: Intel's SDM doesn't mandate the VMCS field be
	 * set when EPT misconfig occurs.  In practice, real hardware updates
	 * VM_EXIT_INSTRUCTION_LEN on EPT misconfig, but other hypervisors
	 * (namely Hyper-V) don't set it due to it being undefined behavior,
	 * i.e. we end up advancing IP with some random value.
	 */
	if (!static_cpu_has(X86_FEATURE_HYPERVISOR) ||
	    exit_reason.basic != EXIT_REASON_EPT_MISCONFIG) {
		instr_len = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);

		/*
		 * Emulating an enclave's instructions isn't supported as KVM
		 * cannot access the enclave's memory or its true RIP, e.g. the
		 * vmcs.GUEST_RIP points at the exit point of the enclave, not
		 * the RIP that actually triggered the VM-Exit.  But, because
		 * most instructions that cause VM-Exit will #UD in an enclave,
		 * most instruction-based VM-Exits simply do not occur.
		 *
		 * There are a few exceptions, notably the debug instructions
		 * INT1ICEBRK and INT3, as they are allowed in debug enclaves
		 * and generate #DB/#BP as expected, which KVM might intercept.
		 * But again, the CPU does the dirty work and saves an instr
		 * length of zero so VMMs don't shoot themselves in the foot.
		 * WARN if KVM tries to skip a non-zero length instruction on
		 * a VM-Exit from an enclave.
		 */
		if (!instr_len)
			goto rip_updated;

		WARN_ONCE(exit_reason.enclave_mode,
			  "skipping instruction after SGX enclave VM-Exit");

		orig_rip = kvm_rip_read(vcpu);
		rip = orig_rip + instr_len;
#ifdef CONFIG_X86_64
		/*
		 * We need to mask out the high 32 bits of RIP if not in 64-bit
		 * mode, but just finding out that we are in 64-bit mode is
		 * quite expensive.  Only do it if there was a carry.
		 */
		if (unlikely(((rip ^ orig_rip) >> 31) == 3) && !is_64_bit_mode(vcpu))
			rip = (u32)rip;
#endif
		kvm_rip_write(vcpu, rip);
	} else {
		if (!kvm_emulate_instruction(vcpu, EMULTYPE_SKIP))
			return 0;
	}

rip_updated:
	/* skipping an emulated instruction also counts */
	vmx_set_interrupt_shadow(vcpu, 0);

	return 1;
}

/*
 * Recognizes a pending MTF VM-exit and records the nested state for later
 * delivery.
 */
void vmx_update_emulated_instruction(struct kvm_vcpu *vcpu)
{
	struct vmcs12 *vmcs12 = get_vmcs12(vcpu);
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!is_guest_mode(vcpu))
		return;

	/*
	 * Per the SDM, MTF takes priority over debug-trap exceptions besides
	 * TSS T-bit traps and ICEBP (INT1).  KVM doesn't emulate T-bit traps
	 * or ICEBP (in the emulator proper), and skipping of ICEBP after an
	 * intercepted #DB deliberately avoids single-step #DB and MTF updates
	 * as ICEBP is higher priority than both.  As instruction emulation is
	 * completed at this point (i.e. KVM is at the instruction boundary),
	 * any #DB exception pending delivery must be a debug-trap of lower
	 * priority than MTF.  Record the pending MTF state to be delivered in
	 * vmx_check_nested_events().
	 */
	if (nested_cpu_has_mtf(vmcs12) &&
	    (!vcpu->arch.exception.pending ||
	     vcpu->arch.exception.vector == DB_VECTOR) &&
	    (!vcpu->arch.exception_vmexit.pending ||
	     vcpu->arch.exception_vmexit.vector == DB_VECTOR)) {
		vmx->nested.mtf_pending = true;
		kvm_make_request(KVM_REQ_EVENT, vcpu);
	} else {
		vmx->nested.mtf_pending = false;
	}
}

int vmx_skip_emulated_instruction(struct kvm_vcpu *vcpu)
{
	vmx_update_emulated_instruction(vcpu);
	return skip_emulated_instruction(vcpu);
}

void vmx_cache_reg(struct kvm_vcpu *vcpu, enum kvm_reg reg)
{
	unsigned long guest_owned_bits;

	kvm_register_mark_available(vcpu, reg);

	switch (reg) {
	case VCPU_REGS_RSP:
		vcpu->arch.regs[VCPU_REGS_RSP] = vmcs_readl(GUEST_RSP);
		break;
	case VCPU_REGS_RIP:
		vcpu->arch.regs[VCPU_REGS_RIP] = vmcs_readl(GUEST_RIP);
		break;
	case VCPU_EXREG_PDPTR:
		if (enable_ept)
			ept_save_pdptrs(vcpu);
		break;
	case VCPU_EXREG_CR0:
		guest_owned_bits = vcpu->arch.cr0_guest_owned_bits;

		vcpu->arch.cr0 &= ~guest_owned_bits;
		vcpu->arch.cr0 |= vmcs_readl(GUEST_CR0) & guest_owned_bits;
		break;
	case VCPU_EXREG_CR3:
		/*
		 * When intercepting CR3 loads, e.g. for shadowing paging, KVM's
		 * CR3 is loaded into hardware, not the guest's CR3.
		 */
		if (!(exec_controls_get(to_vmx(vcpu)) & CPU_BASED_CR3_LOAD_EXITING))
			vcpu->arch.cr3 = vmcs_readl(GUEST_CR3);
		break;
	case VCPU_EXREG_CR4:
		guest_owned_bits = vcpu->arch.cr4_guest_owned_bits;

		vcpu->arch.cr4 &= ~guest_owned_bits;
		vcpu->arch.cr4 |= vmcs_readl(GUEST_CR4) & guest_owned_bits;
		break;
	default:
		KVM_BUG_ON(1, vcpu->kvm);
		break;
	}
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

int vmx_check_processor_compat(void)
{
	int cpu = raw_smp_processor_id();
	struct vmcs_config vmcs_conf;
	struct vmx_capability vmx_cap;

	if (setup_vmcs_config(&vmcs_conf, &vmx_cap) < 0) {
		pr_err("Failed to setup VMCS config on CPU %d\n", cpu);
		return -EIO;
	}

	if (memcmp(&vmcs_config, &vmcs_conf, sizeof(struct vmcs_config))) {
		pr_err("Inconsistent VMCS config on CPU %d\n", cpu);
		return -EIO;
	}

	return 0;
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

/*
 * Free a VMCS, but before that VMCLEAR it on the CPU where it was last loaded.
 */
void free_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	if (!loaded_vmcs->vmcs)
		return;
#ifdef __PKVM_HYP__
	__pkvm_hyp_donate_host(__pkvm_pa(loaded_vmcs->vmcs), PAGE_SIZE);
	/*
	 * The vmcs free may happen on a CPU which didn't load this vmcs and
	 * pkvm hypervisor doesn't have smp call support, so not to do vmcs
	 * clear here but when vcpu is put.
	 */
#else
	loaded_vmcs_clear(loaded_vmcs);
	free_vmcs(loaded_vmcs->vmcs);
#endif
	loaded_vmcs->vmcs = NULL;
	if (loaded_vmcs->msr_bitmap)
#ifdef __PKVM_HYP__
	{
		/*
		 * TODO undoate msr_bitmap page
		__pkvm_hyp_donate_host(__pkvm_pa(loaded_vmcs->msr_bitmap), PAGE_SIZE);
		 */
		;
	}
#else
		free_page((unsigned long)loaded_vmcs->msr_bitmap);
#endif
	WARN_ON(loaded_vmcs->shadow_vmcs != NULL);
}

int alloc_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
#ifndef __PKVM_HYP__
	loaded_vmcs->vmcs = alloc_vmcs(false);
#endif
	if (!loaded_vmcs->vmcs)
		return -ENOMEM;

#ifdef __PKVM_HYP__
	if (__pkvm_host_donate_hyp(__pkvm_pa(loaded_vmcs->vmcs), PAGE_SIZE)) {
		loaded_vmcs->vmcs = NULL;
		loaded_vmcs->msr_bitmap = NULL;
		goto out_vmcs;
	}

	memset(loaded_vmcs->vmcs, 0, vmx_basic_vmcs_size(vmcs_config.basic));
	loaded_vmcs->vmcs->hdr.revision_id = vmx_basic_vmcs_revision_id(vmcs_config.basic);
#endif

	vmcs_clear(loaded_vmcs->vmcs);

	loaded_vmcs->shadow_vmcs = NULL;
	loaded_vmcs->hv_timer_soft_disabled = false;
	loaded_vmcs->cpu = -1;
	loaded_vmcs->launched = 0;

	if (cpu_has_vmx_msr_bitmap()) {
#ifndef __PKVM_HYP__
		loaded_vmcs->msr_bitmap = (unsigned long *)
				__get_free_page(GFP_KERNEL_ACCOUNT);
#endif
		if (!loaded_vmcs->msr_bitmap)
			goto out_vmcs;
#ifdef __PKVM_HYP__
		/*
		 * TODO: donate msr_bitmap. Cannot do this right now as the linux
		 * host may still need to access msr_bitmap.
		if (__pkvm_host_donate_hyp(__pkvm_pa(loaded_vmcs->msr_bitmap),
					   PAGE_SIZE)) {
			loaded_vmcs->msr_bitmap = NULL;
			goto out_vmcs;
		}
		 */
#endif
		memset(loaded_vmcs->msr_bitmap, 0xff, PAGE_SIZE);
	}

	memset(&loaded_vmcs->host_state, 0, sizeof(struct vmcs_host_state));
	memset(&loaded_vmcs->controls_shadow, 0,
		sizeof(struct vmcs_controls_shadow));

	return 0;

out_vmcs:
	free_loaded_vmcs(loaded_vmcs);
	return -ENOMEM;
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

void vmx_flush_tlb_all(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	/*
	 * INVEPT must be issued when EPT is enabled, irrespective of VPID, as
	 * the CPU is not required to invalidate guest-physical mappings on
	 * VM-Entry, even if VPID is disabled.  Guest-physical mappings are
	 * associated with the root EPT structure and not any particular VPID
	 * (INVVPID also isn't required to invalidate guest-physical mappings).
	 */
	if (enable_ept) {
		ept_sync_global();
	} else if (enable_vpid) {
		if (cpu_has_vmx_invvpid_global()) {
			vpid_sync_vcpu_global();
		} else {
			vpid_sync_vcpu_single(vmx->vpid);
			vpid_sync_vcpu_single(vmx->nested.vpid02);
		}
	}
}

static inline int vmx_get_current_vpid(struct kvm_vcpu *vcpu)
{
	if (is_guest_mode(vcpu))
		return nested_get_vpid02(vcpu);
	return to_vmx(vcpu)->vpid;
}

void vmx_flush_tlb_current(struct kvm_vcpu *vcpu)
{
	struct kvm_mmu *mmu = vcpu->arch.mmu;
	u64 root_hpa = mmu->root.hpa;

	/* No flush required if the current context is invalid. */
	if (!VALID_PAGE(root_hpa))
		return;

	if (enable_ept)
		ept_sync_context(construct_eptp(vcpu, root_hpa,
						mmu->root_role.level));
	else
		vpid_sync_context(vmx_get_current_vpid(vcpu));
}

void ept_save_pdptrs(struct kvm_vcpu *vcpu)
{
	struct kvm_mmu *mmu = vcpu->arch.walk_mmu;

	if (WARN_ON_ONCE(!is_pae_paging(vcpu)))
		return;

	mmu->pdptrs[0] = vmcs_read64(GUEST_PDPTR0);
	mmu->pdptrs[1] = vmcs_read64(GUEST_PDPTR1);
	mmu->pdptrs[2] = vmcs_read64(GUEST_PDPTR2);
	mmu->pdptrs[3] = vmcs_read64(GUEST_PDPTR3);

	kvm_register_mark_available(vcpu, VCPU_EXREG_PDPTR);
}

u64 construct_eptp(struct kvm_vcpu *vcpu, hpa_t root_hpa, int root_level)
{
	u64 eptp = VMX_EPTP_MT_WB;

	eptp |= (root_level == 5) ? VMX_EPTP_PWL_5 : VMX_EPTP_PWL_4;

	if (enable_ept_ad_bits &&
	    (!is_guest_mode(vcpu) || nested_ept_ad_enabled(vcpu)))
		eptp |= VMX_EPTP_AD_ENABLE_BIT;
	eptp |= root_hpa;

	return eptp;
}

int vmx_get_cpl(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (unlikely(vmx->rmode.vm86_active))
		return 0;
	else {
		int ar = vmx_read_guest_seg_ar(vmx, VCPU_SREG_SS);
		return VMX_AR_DPL(ar);
	}
}

int allocate_vpid(void)
{
	int vpid;

	if (!enable_vpid)
		return 0;
#ifdef __PKVM_HYP__
	pkvm_spin_lock(&vmx_vpid_lock);
#else
	spin_lock(&vmx_vpid_lock);
#endif
	vpid = find_first_zero_bit(vmx_vpid_bitmap, VMX_NR_VPIDS);
	if (vpid < VMX_NR_VPIDS)
		__set_bit(vpid, vmx_vpid_bitmap);
	else
		vpid = 0;
#ifdef __PKVM_HYP__
	pkvm_spin_unlock(&vmx_vpid_lock);
#else
	spin_unlock(&vmx_vpid_lock);
#endif
	return vpid;
}

void free_vpid(int vpid)
{
	if (!enable_vpid || vpid == 0)
		return;
#ifdef __PKVM_HYP__
	pkvm_spin_lock(&vmx_vpid_lock);
#else
	spin_lock(&vmx_vpid_lock);
#endif
	__clear_bit(vpid, vmx_vpid_bitmap);
#ifdef __PKVM_HYP__
	pkvm_spin_unlock(&vmx_vpid_lock);
#else
	spin_unlock(&vmx_vpid_lock);
#endif
}

static void vmx_msr_bitmap_l01_changed(struct vcpu_vmx *vmx)
{
#ifndef __PKVM_HYP__
	/*
	 * When KVM is a nested hypervisor on top of Hyper-V and uses
	 * 'Enlightened MSR Bitmap' feature L0 needs to know that MSR
	 * bitmap has changed.
	 */
	if (kvm_is_using_evmcs()) {
		struct hv_enlightened_vmcs *evmcs = (void *)vmx->vmcs01.vmcs;

		if (evmcs->hv_enlightenments_control.msr_bitmap)
			evmcs->hv_clean_fields &=
				~HV_VMX_ENLIGHTENED_CLEAN_FIELD_MSR_BITMAP;
	}
#endif

	vmx->nested.force_msr_bitmap_recalc = true;
}

void vmx_disable_intercept_for_msr(struct kvm_vcpu *vcpu, u32 msr, int type)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long *msr_bitmap = vmx->vmcs01.msr_bitmap;
	int idx;

	if (!cpu_has_vmx_msr_bitmap())
		return;

	vmx_msr_bitmap_l01_changed(vmx);

	/*
	 * Mark the desired intercept state in shadow bitmap, this is needed
	 * for resync when the MSR filters change.
	 */
	idx = vmx_get_passthrough_msr_slot(msr);
	if (idx >= 0) {
		if (type & MSR_TYPE_R)
			clear_bit(idx, vmx->shadow_msr_intercept.read);
		if (type & MSR_TYPE_W)
			clear_bit(idx, vmx->shadow_msr_intercept.write);
	}

	if ((type & MSR_TYPE_R) &&
	    !kvm_msr_allowed(vcpu, msr, KVM_MSR_FILTER_READ)) {
		vmx_set_msr_bitmap_read(msr_bitmap, msr);
		type &= ~MSR_TYPE_R;
	}

	if ((type & MSR_TYPE_W) &&
	    !kvm_msr_allowed(vcpu, msr, KVM_MSR_FILTER_WRITE)) {
		vmx_set_msr_bitmap_write(msr_bitmap, msr);
		type &= ~MSR_TYPE_W;
	}

	if (type & MSR_TYPE_R)
		vmx_clear_msr_bitmap_read(msr_bitmap, msr);

	if (type & MSR_TYPE_W)
		vmx_clear_msr_bitmap_write(msr_bitmap, msr);
}

bool __vmx_interrupt_blocked(struct kvm_vcpu *vcpu)
{
	return !(vmx_get_rflags(vcpu) & X86_EFLAGS_IF) ||
	       (vmcs_read32(GUEST_INTERRUPTIBILITY_INFO) &
		(GUEST_INTR_STATE_STI | GUEST_INTR_STATE_MOV_SS));
}

bool vmx_interrupt_blocked(struct kvm_vcpu *vcpu)
{
	if (is_guest_mode(vcpu) && nested_exit_on_intr(vcpu))
		return false;

	return __vmx_interrupt_blocked(vcpu);
}

static int handle_machine_check(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_exception_nmi(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static __always_inline int handle_external_interrupt(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_triple_fault(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

#ifdef __PKVM_HYP__
static int handle_init(struct kvm_vcpu *vcpu)
{
	/*
	 * INIT vmexit reason is unsupported by host KVM and
	 * it is reused by pkvm to kick vcpu out of non-root.
	 * When this vmexit reason happens, no need back to host.
	 */
	return 1;
}
#endif

static int handle_io(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_desc(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_cr(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_dr(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_tpr_below_threshold(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_interrupt_window(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_invlpg(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

#ifdef __PKVM_HYP__
static int kvm_pkvm_hypercall(struct kvm_vcpu *vcpu)
{
	u64 nr, a0, a1, a2, a3;
	struct shadow_vcpu_state *shadow_vcpu = kvm_vcpu_to_shadow(vcpu);
	struct pkvm_pgtable *pgstate_pgt = &shadow_vcpu->vm->pgstate_pgt;
	int cpl = vmx_get_cpl(vcpu);
	int ret = -KVM_EPERM;

	if (cpl) {
		/* TODO: inject #GP */
		return 1;
	}

	nr = kvm_rax_read(vcpu);
	a0 = kvm_rbx_read(vcpu);
	a1 = kvm_rcx_read(vcpu);
	a2 = kvm_rdx_read(vcpu);
	a3 = kvm_rsi_read(vcpu);

	switch (nr) {
	case PKVM_GHC_SHARE_MEM:
		ret = __pkvm_guest_share_host(pgstate_pgt, a0, a1);
		break;
	case PKVM_GHC_UNSHARE_MEM:
		ret = __pkvm_guest_unshare_host(pgstate_pgt, a0, a1);
		break;
	case PKVM_GHC_IOREAD:
	case PKVM_GHC_IOWRITE:
		/* Hypercall for MMIO accessing should be forwared to the host */
		return 0;
	default:
		/* The other hypercalls are not supported */
		break;
	}

	kvm_rax_write(vcpu, ret);

	return kvm_skip_emulated_instruction(vcpu);
}
#endif

static int handle_vmcall(struct kvm_vcpu *vcpu)
{
#ifdef __PKVM_HYP__
	if (pkvm_is_protected_vcpu(vcpu))
		return kvm_pkvm_hypercall(vcpu);
#endif
	return kvm_emulate_hypercall(vcpu);
}

static int handle_apic_access(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_apic_eoi_induced(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_apic_write(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_task_switch(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_ept_violation(struct kvm_vcpu *vcpu)
{
	unsigned long exit_qualification;
	gpa_t gpa;
	u64 error_code;

	exit_qualification = vmx_get_exit_qual(vcpu);

	/*
	 * EPT violation happened while executing iret from NMI,
	 * "blocked by NMI" bit has to be set before next VM entry.
	 * There are errata that may cause this bit to not be set:
	 * AAK134, BY25.
	 */
	if (!(to_vmx(vcpu)->idt_vectoring_info & VECTORING_INFO_VALID_MASK) &&
			enable_vnmi &&
			(exit_qualification & INTR_INFO_UNBLOCK_NMI))
		vmcs_set_bits(GUEST_INTERRUPTIBILITY_INFO, GUEST_INTR_STATE_NMI);

	gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);
	trace_kvm_page_fault(vcpu, gpa, exit_qualification);

	/* Is it a read fault? */
	error_code = (exit_qualification & EPT_VIOLATION_ACC_READ)
		     ? PFERR_USER_MASK : 0;
	/* Is it a write fault? */
	error_code |= (exit_qualification & EPT_VIOLATION_ACC_WRITE)
		      ? PFERR_WRITE_MASK : 0;
	/* Is it a fetch fault? */
	error_code |= (exit_qualification & EPT_VIOLATION_ACC_INSTR)
		      ? PFERR_FETCH_MASK : 0;
	/* ept page table entry is present? */
	error_code |= (exit_qualification & EPT_VIOLATION_RWX_MASK)
		      ? PFERR_PRESENT_MASK : 0;

	error_code |= (exit_qualification & EPT_VIOLATION_GVA_TRANSLATED) != 0 ?
	       PFERR_GUEST_FINAL_MASK : PFERR_GUEST_PAGE_MASK;

	/*
	 * Check that the GPA doesn't exceed physical memory limits, as that is
	 * a guest page fault.  We have to emulate the instruction here, because
	 * if the illegal address is that of a paging structure, then
	 * EPT_VIOLATION_ACC_WRITE bit is set.  Alternatively, if supported we
	 * would also use advanced VM-exit information for EPT violations to
	 * reconstruct the page fault error code.
	 */
	if (unlikely(allow_smaller_maxphyaddr && !kvm_vcpu_is_legal_gpa(vcpu, gpa)))
		return kvm_emulate_instruction(vcpu, 0);

#ifdef __PKVM_HYP__
	return pkvm_handle_guest_ept_violation(vcpu, gpa);
#else
	return kvm_mmu_page_fault(vcpu, gpa, error_code, NULL, 0);
#endif
}

static int handle_ept_misconfig(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_nmi_window(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_invalid_guest_state(struct kvm_vcpu *vcpu)
{
#ifdef __PKVM_HYP__
	/*
	 * vmx->emulation_required is determined by vmx_emulation_required()
	 * which will check is_unrestricted_guest or not. For the pkvm
	 * hypervisor, it requires unrestricted_guest feature thus
	 * emulation_required is always false, so should not be here to handle
	 * invalid guest state.
	 */
	return 1;
#else
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	bool intr_window_requested;
	unsigned count = 130;

	intr_window_requested = exec_controls_get(vmx) &
				CPU_BASED_INTR_WINDOW_EXITING;

	while (vmx->emulation_required && count-- != 0) {
		if (intr_window_requested && !vmx_interrupt_blocked(vcpu))
			return handle_interrupt_window(&vmx->vcpu);

		if (kvm_test_request(KVM_REQ_EVENT, vcpu))
			return 1;

		if (!kvm_emulate_instruction(vcpu, 0))
			return 0;

		if (vmx_emulation_required_with_pending_exception(vcpu)) {
			kvm_prepare_emulation_failure_exit(vcpu);
			return 0;
		}

		if (vcpu->arch.halt_request) {
			vcpu->arch.halt_request = 0;
			return kvm_emulate_halt_noskip(vcpu);
		}

		/*
		 * Note, return 1 and not 0, vcpu_run() will invoke
		 * xfer_to_guest_mode() which will create a proper return
		 * code.
		 */
		if (__xfer_to_guest_mode_work_pending())
			return 1;
	}

	return 1;
#endif
}

static bool vmx_emulation_required_with_pending_exception(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	return vmx->emulation_required && !vmx->rmode.vm86_active &&
	       (kvm_is_exception_pending(vcpu) || vcpu->arch.exception.injected);
}

int vmx_vcpu_pre_run(struct kvm_vcpu *vcpu)
{
	if (vmx_emulation_required_with_pending_exception(vcpu)) {
#ifndef __PKVM_HYP__
		kvm_prepare_emulation_failure_exit(vcpu);
#endif
		return 0;
	}
#ifdef __PKVM_HYP__
	if (pkvm_is_protected_vcpu(vcpu)) {
		struct shadow_vcpu_state *shadow_vcpu = kvm_vcpu_to_shadow(vcpu);

		if (!READ_ONCE(shadow_vcpu->allowed_to_run))
			return 0;

		/*
		 * Ensure that pvmfw_entry_pending and pvmfw_load_addr are read
		 * after allowed_to_run, so they are read with up-to-date values.
		 * Paired with __smp_wmb() in __pkvm_finalize_shadow_vm().
		 */
		__smp_rmb();

		if (shadow_vcpu->pvmfw_entry_pending) {
			kvm_rip_write(vcpu, shadow_vcpu->vm->pvmfw_load_addr);
			shadow_vcpu->pvmfw_entry_pending = false;
		}
	}
	pkvm_update_host_rip(vcpu);
#endif

	return 1;
}

static int handle_pause(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_monitor_trap(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_invpcid(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_pml_full(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static fastpath_t handle_fastpath_preemption_timer(struct kvm_vcpu *vcpu,
						   bool force_immediate_exit)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	/*
	 * In the *extremely* unlikely scenario that this is a spurious VM-Exit
	 * due to the timer expiring while it was "soft" disabled, just eat the
	 * exit and re-enter the guest.
	 */
	if (unlikely(vmx->loaded_vmcs->hv_timer_soft_disabled))
		return EXIT_FASTPATH_REENTER_GUEST;

	/*
	 * If the timer expired because KVM used it to force an immediate exit,
	 * then mission accomplished.
	 */
	if (force_immediate_exit)
		return EXIT_FASTPATH_EXIT_HANDLED;

#ifdef __PKVM_HYP__
	return EXIT_FASTPATH_NONE;
#else
	/*
	 * If L2 is active, go down the slow path as emulating the guest timer
	 * expiration likely requires synthesizing a nested VM-Exit.
	 */
	if (is_guest_mode(vcpu))
		return EXIT_FASTPATH_NONE;

	kvm_lapic_expired_hv_timer(vcpu);
	return EXIT_FASTPATH_REENTER_GUEST;
#endif
}

static int handle_preemption_timer(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_vmx_instruction(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_encls(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_bus_lock_vmexit(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

static int handle_notify(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}

/*
 * The exit handlers return 1 if the exit was handled fully and guest execution
 * may resume.  Otherwise they set the kvm_run parameter to indicate what needs
 * to be done to userspace and return 0.
 */
static int (*kvm_vmx_exit_handlers[])(struct kvm_vcpu *vcpu) = {
	[EXIT_REASON_EXCEPTION_NMI]           = handle_exception_nmi,
	[EXIT_REASON_EXTERNAL_INTERRUPT]      = handle_external_interrupt,
	[EXIT_REASON_TRIPLE_FAULT]            = handle_triple_fault,
#ifdef __PKVM_HYP__
	[EXIT_REASON_INIT_SIGNAL]	      = handle_init,
#endif
	[EXIT_REASON_NMI_WINDOW]	      = handle_nmi_window,
	[EXIT_REASON_IO_INSTRUCTION]          = handle_io,
	[EXIT_REASON_CR_ACCESS]               = handle_cr,
	[EXIT_REASON_DR_ACCESS]               = handle_dr,
	[EXIT_REASON_CPUID]                   = kvm_emulate_cpuid,
	[EXIT_REASON_MSR_READ]                = kvm_emulate_rdmsr,
	[EXIT_REASON_MSR_WRITE]               = kvm_emulate_wrmsr,
	[EXIT_REASON_INTERRUPT_WINDOW]        = handle_interrupt_window,
	[EXIT_REASON_HLT]                     = kvm_emulate_halt,
	[EXIT_REASON_INVD]		      = kvm_emulate_invd,
	[EXIT_REASON_INVLPG]		      = handle_invlpg,
	[EXIT_REASON_RDPMC]                   = kvm_emulate_rdpmc,
	[EXIT_REASON_VMCALL]                  = handle_vmcall,
	[EXIT_REASON_VMCLEAR]		      = handle_vmx_instruction,
	[EXIT_REASON_VMLAUNCH]		      = handle_vmx_instruction,
	[EXIT_REASON_VMPTRLD]		      = handle_vmx_instruction,
	[EXIT_REASON_VMPTRST]		      = handle_vmx_instruction,
	[EXIT_REASON_VMREAD]		      = handle_vmx_instruction,
	[EXIT_REASON_VMRESUME]		      = handle_vmx_instruction,
	[EXIT_REASON_VMWRITE]		      = handle_vmx_instruction,
	[EXIT_REASON_VMOFF]		      = handle_vmx_instruction,
	[EXIT_REASON_VMON]		      = handle_vmx_instruction,
	[EXIT_REASON_TPR_BELOW_THRESHOLD]     = handle_tpr_below_threshold,
	[EXIT_REASON_APIC_ACCESS]             = handle_apic_access,
	[EXIT_REASON_APIC_WRITE]              = handle_apic_write,
	[EXIT_REASON_EOI_INDUCED]             = handle_apic_eoi_induced,
	[EXIT_REASON_WBINVD]                  = kvm_emulate_wbinvd,
	[EXIT_REASON_XSETBV]                  = kvm_emulate_xsetbv,
	[EXIT_REASON_TASK_SWITCH]             = handle_task_switch,
	[EXIT_REASON_MCE_DURING_VMENTRY]      = handle_machine_check,
	[EXIT_REASON_GDTR_IDTR]		      = handle_desc,
	[EXIT_REASON_LDTR_TR]		      = handle_desc,
	[EXIT_REASON_EPT_VIOLATION]	      = handle_ept_violation,
	[EXIT_REASON_EPT_MISCONFIG]           = handle_ept_misconfig,
	[EXIT_REASON_PAUSE_INSTRUCTION]       = handle_pause,
	[EXIT_REASON_MWAIT_INSTRUCTION]	      = kvm_emulate_mwait,
	[EXIT_REASON_MONITOR_TRAP_FLAG]       = handle_monitor_trap,
	[EXIT_REASON_MONITOR_INSTRUCTION]     = kvm_emulate_monitor,
	[EXIT_REASON_INVEPT]                  = handle_vmx_instruction,
	[EXIT_REASON_INVVPID]                 = handle_vmx_instruction,
	[EXIT_REASON_RDRAND]                  = kvm_handle_invalid_op,
	[EXIT_REASON_RDSEED]                  = kvm_handle_invalid_op,
	[EXIT_REASON_PML_FULL]		      = handle_pml_full,
	[EXIT_REASON_INVPCID]                 = handle_invpcid,
	[EXIT_REASON_VMFUNC]		      = handle_vmx_instruction,
	[EXIT_REASON_PREEMPTION_TIMER]	      = handle_preemption_timer,
	[EXIT_REASON_ENCLS]		      = handle_encls,
	[EXIT_REASON_BUS_LOCK]                = handle_bus_lock_vmexit,
	[EXIT_REASON_NOTIFY]		      = handle_notify,
};

static const int kvm_vmx_max_exit_handlers =
	ARRAY_SIZE(kvm_vmx_exit_handlers);

static void vmx_destroy_pml_buffer(struct vcpu_vmx *vmx)
{
	if (vmx->pml_pg) {
#ifdef __PKVM_HYP__
		/*
		 * TODO: undoate pml_pg.
		__pkvm_hyp_donate_host(__pkvm_pa(vmx->pml_pg), PAGE_SIZE);
		 */
#else
		free_page((unsigned long)vmx->pml_pg);
#endif
		vmx->pml_pg = NULL;
	}
}

static void vmx_destroy_ve(struct vcpu_vmx *vmx)
{
	if (vmx->ve_info) {
#ifdef __PKVM_HYP__
		/*
		 * TODO: undoate ve_info page
		__pkvm_hyp_donate_host(__pkvm_pa(vmx->ve_info), PAGE_SIZE);
		 */
#else
		free_page((unsigned long)vmx->ve_info);
#endif
		vmx->ve_info = NULL;
	}
}

static void vmx_flush_pml_buffer(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	u64 *pml_buf;
	u16 pml_idx;

	pml_idx = vmcs_read16(GUEST_PML_INDEX);

	/* Do nothing if PML buffer is empty */
	if (pml_idx == (PML_ENTITY_NUM - 1))
		return;

	/* PML index always points to next available PML buffer entity */
	if (pml_idx >= PML_ENTITY_NUM)
		pml_idx = 0;
	else
		pml_idx++;

	pml_buf = (u64 *)vmx->pml_pg;
	for (; pml_idx < PML_ENTITY_NUM; pml_idx++) {
		u64 gpa;

		gpa = pml_buf[pml_idx];
		WARN_ON(gpa & (PAGE_SIZE - 1));
		/*
		 * FIXME: Flushing PML needs to mark dirty page, which is not supported
		 * by the pkvm hypervisor. Needs to see how to support this with the PV
		 * method in the future.
		 */
#ifndef __PKVM_HYP__
		kvm_vcpu_mark_page_dirty(vcpu, gpa >> PAGE_SHIFT);
#endif
	}

	/* reset PML index */
	vmcs_write16(GUEST_PML_INDEX, PML_ENTITY_NUM - 1);
}

static void vmx_dump_sel(char *name, uint32_t sel)
{
	pr_err("%s sel=0x%04x, attr=0x%05x, limit=0x%08x, base=0x%016lx\n",
	       name, vmcs_read16(sel),
	       vmcs_read32(sel + GUEST_ES_AR_BYTES - GUEST_ES_SELECTOR),
	       vmcs_read32(sel + GUEST_ES_LIMIT - GUEST_ES_SELECTOR),
	       vmcs_readl(sel + GUEST_ES_BASE - GUEST_ES_SELECTOR));
}

static void vmx_dump_dtsel(char *name, uint32_t limit)
{
	pr_err("%s                           limit=0x%08x, base=0x%016lx\n",
	       name, vmcs_read32(limit),
	       vmcs_readl(limit + GUEST_GDTR_BASE - GUEST_GDTR_LIMIT));
}

static void vmx_dump_msrs(char *name, struct vmx_msrs *m)
{
	unsigned int i;
	struct vmx_msr_entry *e;

	pr_err("MSR %s:\n", name);
	for (i = 0, e = m->val; i < m->nr; ++i, ++e)
		pr_err("  %2d: msr=0x%08x value=0x%016llx\n", i, e->index, e->value);
}

void dump_vmcs(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	u32 vmentry_ctl, vmexit_ctl;
	u32 cpu_based_exec_ctrl, pin_based_exec_ctrl, secondary_exec_control;
	u64 tertiary_exec_control;
	unsigned long cr4;
	int efer_slot;

	if (!dump_invalid_vmcs) {
		pr_warn_ratelimited("set kvm_intel.dump_invalid_vmcs=1 to dump internal KVM state.\n");
		return;
	}

	vmentry_ctl = vmcs_read32(VM_ENTRY_CONTROLS);
	vmexit_ctl = vmcs_read32(VM_EXIT_CONTROLS);
	cpu_based_exec_ctrl = vmcs_read32(CPU_BASED_VM_EXEC_CONTROL);
	pin_based_exec_ctrl = vmcs_read32(PIN_BASED_VM_EXEC_CONTROL);
	cr4 = vmcs_readl(GUEST_CR4);

	if (cpu_has_secondary_exec_ctrls())
		secondary_exec_control = vmcs_read32(SECONDARY_VM_EXEC_CONTROL);
	else
		secondary_exec_control = 0;

	if (cpu_has_tertiary_exec_ctrls())
		tertiary_exec_control = vmcs_read64(TERTIARY_VM_EXEC_CONTROL);
	else
		tertiary_exec_control = 0;

	pr_err("VMCS %p, last attempted VM-entry on CPU %d\n",
	       vmx->loaded_vmcs->vmcs, vcpu->arch.last_vmentry_cpu);
	pr_err("*** Guest State ***\n");
	pr_err("CR0: actual=0x%016lx, shadow=0x%016lx, gh_mask=%016lx\n",
	       vmcs_readl(GUEST_CR0), vmcs_readl(CR0_READ_SHADOW),
	       vmcs_readl(CR0_GUEST_HOST_MASK));
	pr_err("CR4: actual=0x%016lx, shadow=0x%016lx, gh_mask=%016lx\n",
	       cr4, vmcs_readl(CR4_READ_SHADOW), vmcs_readl(CR4_GUEST_HOST_MASK));
	pr_err("CR3 = 0x%016lx\n", vmcs_readl(GUEST_CR3));
	if (cpu_has_vmx_ept()) {
		pr_err("PDPTR0 = 0x%016llx  PDPTR1 = 0x%016llx\n",
		       vmcs_read64(GUEST_PDPTR0), vmcs_read64(GUEST_PDPTR1));
		pr_err("PDPTR2 = 0x%016llx  PDPTR3 = 0x%016llx\n",
		       vmcs_read64(GUEST_PDPTR2), vmcs_read64(GUEST_PDPTR3));
	}
	pr_err("RSP = 0x%016lx  RIP = 0x%016lx\n",
	       vmcs_readl(GUEST_RSP), vmcs_readl(GUEST_RIP));
	pr_err("RFLAGS=0x%08lx         DR7 = 0x%016lx\n",
	       vmcs_readl(GUEST_RFLAGS), vmcs_readl(GUEST_DR7));
	pr_err("Sysenter RSP=%016lx CS:RIP=%04x:%016lx\n",
	       vmcs_readl(GUEST_SYSENTER_ESP),
	       vmcs_read32(GUEST_SYSENTER_CS), vmcs_readl(GUEST_SYSENTER_EIP));
	vmx_dump_sel("CS:  ", GUEST_CS_SELECTOR);
	vmx_dump_sel("DS:  ", GUEST_DS_SELECTOR);
	vmx_dump_sel("SS:  ", GUEST_SS_SELECTOR);
	vmx_dump_sel("ES:  ", GUEST_ES_SELECTOR);
	vmx_dump_sel("FS:  ", GUEST_FS_SELECTOR);
	vmx_dump_sel("GS:  ", GUEST_GS_SELECTOR);
	vmx_dump_dtsel("GDTR:", GUEST_GDTR_LIMIT);
	vmx_dump_sel("LDTR:", GUEST_LDTR_SELECTOR);
	vmx_dump_dtsel("IDTR:", GUEST_IDTR_LIMIT);
	vmx_dump_sel("TR:  ", GUEST_TR_SELECTOR);
	efer_slot = vmx_find_loadstore_msr_slot(&vmx->msr_autoload.guest, MSR_EFER);
	if (vmentry_ctl & VM_ENTRY_LOAD_IA32_EFER)
		pr_err("EFER= 0x%016llx 0x%016llx\n", vmcs_read64(GUEST_IA32_EFER), vcpu->arch.efer);
	else if (efer_slot >= 0)
		pr_err("EFER= 0x%016llx (autoload)\n",
		       vmx->msr_autoload.guest.val[efer_slot].value);
	else if (vmentry_ctl & VM_ENTRY_IA32E_MODE)
		pr_err("EFER= 0x%016llx (effective)\n",
		       vcpu->arch.efer | (EFER_LMA | EFER_LME));
	else
		pr_err("EFER= 0x%016llx (effective)\n",
		       vcpu->arch.efer & ~(EFER_LMA | EFER_LME));
	if (vmentry_ctl & VM_ENTRY_LOAD_IA32_PAT)
		pr_err("PAT = 0x%016llx\n", vmcs_read64(GUEST_IA32_PAT));
	pr_err("DebugCtl = 0x%016llx  DebugExceptions = 0x%016lx\n",
	       vmcs_read64(GUEST_IA32_DEBUGCTL),
	       vmcs_readl(GUEST_PENDING_DBG_EXCEPTIONS));
	if (cpu_has_load_perf_global_ctrl() &&
	    vmentry_ctl & VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL)
		pr_err("PerfGlobCtl = 0x%016llx\n",
		       vmcs_read64(GUEST_IA32_PERF_GLOBAL_CTRL));
	if (vmentry_ctl & VM_ENTRY_LOAD_BNDCFGS)
		pr_err("BndCfgS = 0x%016llx\n", vmcs_read64(GUEST_BNDCFGS));
	pr_err("Interruptibility = %08x  ActivityState = %08x\n",
	       vmcs_read32(GUEST_INTERRUPTIBILITY_INFO),
	       vmcs_read32(GUEST_ACTIVITY_STATE));
	if (secondary_exec_control & SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY)
		pr_err("InterruptStatus = %04x\n",
		       vmcs_read16(GUEST_INTR_STATUS));
	if (vmcs_read32(VM_ENTRY_MSR_LOAD_COUNT) > 0)
		vmx_dump_msrs("guest autoload", &vmx->msr_autoload.guest);
	if (vmcs_read32(VM_EXIT_MSR_STORE_COUNT) > 0)
		vmx_dump_msrs("guest autostore", &vmx->msr_autostore.guest);

	pr_err("*** Host State ***\n");
	pr_err("RIP = 0x%016lx  RSP = 0x%016lx\n",
	       vmcs_readl(HOST_RIP), vmcs_readl(HOST_RSP));
	pr_err("CS=%04x SS=%04x DS=%04x ES=%04x FS=%04x GS=%04x TR=%04x\n",
	       vmcs_read16(HOST_CS_SELECTOR), vmcs_read16(HOST_SS_SELECTOR),
	       vmcs_read16(HOST_DS_SELECTOR), vmcs_read16(HOST_ES_SELECTOR),
	       vmcs_read16(HOST_FS_SELECTOR), vmcs_read16(HOST_GS_SELECTOR),
	       vmcs_read16(HOST_TR_SELECTOR));
	pr_err("FSBase=%016lx GSBase=%016lx TRBase=%016lx\n",
	       vmcs_readl(HOST_FS_BASE), vmcs_readl(HOST_GS_BASE),
	       vmcs_readl(HOST_TR_BASE));
	pr_err("GDTBase=%016lx IDTBase=%016lx\n",
	       vmcs_readl(HOST_GDTR_BASE), vmcs_readl(HOST_IDTR_BASE));
	pr_err("CR0=%016lx CR3=%016lx CR4=%016lx\n",
	       vmcs_readl(HOST_CR0), vmcs_readl(HOST_CR3),
	       vmcs_readl(HOST_CR4));
	pr_err("Sysenter RSP=%016lx CS:RIP=%04x:%016lx\n",
	       vmcs_readl(HOST_IA32_SYSENTER_ESP),
	       vmcs_read32(HOST_IA32_SYSENTER_CS),
	       vmcs_readl(HOST_IA32_SYSENTER_EIP));
	if (vmexit_ctl & VM_EXIT_LOAD_IA32_EFER)
		pr_err("EFER= 0x%016llx\n", vmcs_read64(HOST_IA32_EFER));
	if (vmexit_ctl & VM_EXIT_LOAD_IA32_PAT)
		pr_err("PAT = 0x%016llx\n", vmcs_read64(HOST_IA32_PAT));
	if (cpu_has_load_perf_global_ctrl() &&
	    vmexit_ctl & VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL)
		pr_err("PerfGlobCtl = 0x%016llx\n",
		       vmcs_read64(HOST_IA32_PERF_GLOBAL_CTRL));
	if (vmcs_read32(VM_EXIT_MSR_LOAD_COUNT) > 0)
		vmx_dump_msrs("host autoload", &vmx->msr_autoload.host);

	pr_err("*** Control State ***\n");
	pr_err("CPUBased=0x%08x SecondaryExec=0x%08x TertiaryExec=0x%016llx\n",
	       cpu_based_exec_ctrl, secondary_exec_control, tertiary_exec_control);
	pr_err("PinBased=0x%08x EntryControls=%08x ExitControls=%08x\n",
	       pin_based_exec_ctrl, vmentry_ctl, vmexit_ctl);
	pr_err("EntryControls_shadow =%08x\n", vmx->loaded_vmcs->controls_shadow.vm_entry);
	pr_err("ExceptionBitmap=%08x PFECmask=%08x PFECmatch=%08x\n",
	       vmcs_read32(EXCEPTION_BITMAP),
	       vmcs_read32(PAGE_FAULT_ERROR_CODE_MASK),
	       vmcs_read32(PAGE_FAULT_ERROR_CODE_MATCH));
	pr_err("VMEntry: intr_info=%08x errcode=%08x ilen=%08x\n",
	       vmcs_read32(VM_ENTRY_INTR_INFO_FIELD),
	       vmcs_read32(VM_ENTRY_EXCEPTION_ERROR_CODE),
	       vmcs_read32(VM_ENTRY_INSTRUCTION_LEN));
	pr_err("VMExit: intr_info=%08x errcode=%08x ilen=%08x\n",
	       vmcs_read32(VM_EXIT_INTR_INFO),
	       vmcs_read32(VM_EXIT_INTR_ERROR_CODE),
	       vmcs_read32(VM_EXIT_INSTRUCTION_LEN));
	pr_err("        reason=%08x qualification=%016lx\n",
	       vmcs_read32(VM_EXIT_REASON), vmcs_readl(EXIT_QUALIFICATION));
	pr_err("IDTVectoring: info=%08x errcode=%08x\n",
	       vmcs_read32(IDT_VECTORING_INFO_FIELD),
	       vmcs_read32(IDT_VECTORING_ERROR_CODE));
	pr_err("TSC Offset = 0x%016llx\n", vmcs_read64(TSC_OFFSET));
	if (secondary_exec_control & SECONDARY_EXEC_TSC_SCALING)
		pr_err("TSC Multiplier = 0x%016llx\n",
		       vmcs_read64(TSC_MULTIPLIER));
	if (cpu_based_exec_ctrl & CPU_BASED_TPR_SHADOW) {
		if (secondary_exec_control & SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY) {
			u16 status = vmcs_read16(GUEST_INTR_STATUS);
			pr_err("SVI|RVI = %02x|%02x ", status >> 8, status & 0xff);
		}
		pr_cont("TPR Threshold = 0x%02x\n", vmcs_read32(TPR_THRESHOLD));
		if (secondary_exec_control & SECONDARY_EXEC_VIRTUALIZE_APIC_ACCESSES)
			pr_err("APIC-access addr = 0x%016llx ", vmcs_read64(APIC_ACCESS_ADDR));
		pr_cont("virt-APIC addr = 0x%016llx\n", vmcs_read64(VIRTUAL_APIC_PAGE_ADDR));
	}
	if (pin_based_exec_ctrl & PIN_BASED_POSTED_INTR)
		pr_err("PostedIntrVec = 0x%02x\n", vmcs_read16(POSTED_INTR_NV));
	if ((secondary_exec_control & SECONDARY_EXEC_ENABLE_EPT))
		pr_err("EPT pointer = 0x%016llx\n", vmcs_read64(EPT_POINTER));
	if (secondary_exec_control & SECONDARY_EXEC_PAUSE_LOOP_EXITING)
		pr_err("PLE Gap=%08x Window=%08x\n",
		       vmcs_read32(PLE_GAP), vmcs_read32(PLE_WINDOW));
	if (secondary_exec_control & SECONDARY_EXEC_ENABLE_VPID)
		pr_err("Virtual processor ID = 0x%04x\n",
		       vmcs_read16(VIRTUAL_PROCESSOR_ID));
	if (secondary_exec_control & SECONDARY_EXEC_EPT_VIOLATION_VE) {
		struct vmx_ve_information *ve_info = vmx->ve_info;
		u64 ve_info_pa = vmcs_read64(VE_INFORMATION_ADDRESS);

		/*
		 * If KVM is dumping the VMCS, then something has gone wrong
		 * already.  Derefencing an address from the VMCS, which could
		 * very well be corrupted, is a terrible idea.  The virtual
		 * address is known so use it.
		 */
		pr_err("VE info address = 0x%016llx%s\n", ve_info_pa,
		       ve_info_pa == __pa(ve_info) ? "" : "(corrupted!)");
		pr_err("ve_info: 0x%08x 0x%08x 0x%016llx 0x%016llx 0x%016llx 0x%04x\n",
		       ve_info->exit_reason, ve_info->delivery,
		       ve_info->exit_qualification,
		       ve_info->guest_linear_address,
		       ve_info->guest_physical_address, ve_info->eptp_index);
	}
}

static int __vmx_handle_exit(struct kvm_vcpu *vcpu, fastpath_t exit_fastpath)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	union vmx_exit_reason exit_reason = vmx->exit_reason;
	u32 vectoring_info = vmx->idt_vectoring_info;
	u16 exit_handler_index;

	/*
	 * Flush logged GPAs PML buffer, this will make dirty_bitmap more
	 * updated. Another good is, in kvm_vm_ioctl_get_dirty_log, before
	 * querying dirty_bitmap, we only need to kick all vcpus out of guest
	 * mode as if vcpus is in root mode, the PML buffer must has been
	 * flushed already.  Note, PML is never enabled in hardware while
	 * running L2.
	 */
	if (enable_pml && !is_guest_mode(vcpu))
		vmx_flush_pml_buffer(vcpu);

	/*
	 * KVM should never reach this point with a pending nested VM-Enter.
	 * More specifically, short-circuiting VM-Entry to emulate L2 due to
	 * invalid guest state should never happen as that means KVM knowingly
	 * allowed a nested VM-Enter with an invalid vmcs12.  More below.
	 */
	if (KVM_BUG_ON(vmx->nested.nested_run_pending, vcpu->kvm))
		return -EIO;

	if (is_guest_mode(vcpu)) {
		/*
		 * PML is never enabled when running L2, bail immediately if a
		 * PML full exit occurs as something is horribly wrong.
		 */
		if (exit_reason.basic == EXIT_REASON_PML_FULL)
			goto unexpected_vmexit;

		/*
		 * The host physical addresses of some pages of guest memory
		 * are loaded into the vmcs02 (e.g. vmcs12's Virtual APIC
		 * Page). The CPU may write to these pages via their host
		 * physical address while L2 is running, bypassing any
		 * address-translation-based dirty tracking (e.g. EPT write
		 * protection).
		 *
		 * Mark them dirty on every exit from L2 to prevent them from
		 * getting out of sync with dirty tracking.
		 */
		nested_mark_vmcs12_pages_dirty(vcpu);

		/*
		 * Synthesize a triple fault if L2 state is invalid.  In normal
		 * operation, nested VM-Enter rejects any attempt to enter L2
		 * with invalid state.  However, those checks are skipped if
		 * state is being stuffed via RSM or KVM_SET_NESTED_STATE.  If
		 * L2 state is invalid, it means either L1 modified SMRAM state
		 * or userspace provided bad state.  Synthesize TRIPLE_FAULT as
		 * doing so is architecturally allowed in the RSM case, and is
		 * the least awful solution for the userspace case without
		 * risking false positives.
		 */
		if (vmx->emulation_required) {
			nested_vmx_vmexit(vcpu, EXIT_REASON_TRIPLE_FAULT, 0, 0);
			return 1;
		}

		if (nested_vmx_reflect_vmexit(vcpu))
			return 1;
	}

	/* If guest state is invalid, start emulating.  L2 is handled above. */
	if (vmx->emulation_required)
		return handle_invalid_guest_state(vcpu);

	if (exit_reason.failed_vmentry) {
		dump_vmcs(vcpu);
#ifndef __PKVM_HYP__ /* No vcpu run page for the vcpu in pkvm hypervisor */
		vcpu->run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		vcpu->run->fail_entry.hardware_entry_failure_reason
			= exit_reason.full;
		vcpu->run->fail_entry.cpu = vcpu->arch.last_vmentry_cpu;
#endif
		return 0;
	}

	if (unlikely(vmx->fail)) {
		dump_vmcs(vcpu);
#ifndef __PKVM_HYP__ /* No vcpu run page for the vcpu in pkvm hypervisor */
		vcpu->run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		vcpu->run->fail_entry.hardware_entry_failure_reason
			= vmcs_read32(VM_INSTRUCTION_ERROR);
		vcpu->run->fail_entry.cpu = vcpu->arch.last_vmentry_cpu;
#endif
		return 0;
	}

	/*
	 * Note:
	 * Do not try to fix EXIT_REASON_EPT_MISCONFIG if it caused by
	 * delivery event since it indicates guest is accessing MMIO.
	 * The vm-exit can be triggered again after return to guest that
	 * will cause infinite loop.
	 */
	if ((vectoring_info & VECTORING_INFO_VALID_MASK) &&
	    (exit_reason.basic != EXIT_REASON_EXCEPTION_NMI &&
	     exit_reason.basic != EXIT_REASON_EPT_VIOLATION &&
	     exit_reason.basic != EXIT_REASON_PML_FULL &&
	     exit_reason.basic != EXIT_REASON_APIC_ACCESS &&
	     exit_reason.basic != EXIT_REASON_TASK_SWITCH &&
	     exit_reason.basic != EXIT_REASON_NOTIFY)) {
#ifndef __PKVM_HYP__ /* No vcpu run page for the vcpu in pkvm hypervisor */
		int ndata = 3;

		vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		vcpu->run->internal.suberror = KVM_INTERNAL_ERROR_DELIVERY_EV;
		vcpu->run->internal.data[0] = vectoring_info;
		vcpu->run->internal.data[1] = exit_reason.full;
		vcpu->run->internal.data[2] = vmx_get_exit_qual(vcpu);
		if (exit_reason.basic == EXIT_REASON_EPT_MISCONFIG) {
			vcpu->run->internal.data[ndata++] =
				vmcs_read64(GUEST_PHYSICAL_ADDRESS);
		}
		vcpu->run->internal.data[ndata++] = vcpu->arch.last_vmentry_cpu;
		vcpu->run->internal.ndata = ndata;
#endif
		return 0;
	}

	if (unlikely(!enable_vnmi &&
		     vmx->loaded_vmcs->soft_vnmi_blocked)) {
		if (!vmx_interrupt_blocked(vcpu)) {
			vmx->loaded_vmcs->soft_vnmi_blocked = 0;
		} else if (vmx->loaded_vmcs->vnmi_blocked_time > 1000000000LL &&
			   vcpu->arch.nmi_pending) {
			/*
			 * This CPU don't support us in finding the end of an
			 * NMI-blocked window if the guest runs with IRQs
			 * disabled. So we pull the trigger after 1 s of
			 * futile waiting, but inform the user about this.
			 */
			printk(KERN_WARNING "%s: Breaking out of NMI-blocked "
			       "state on VCPU %d after 1 s timeout\n",
			       __func__, vcpu->vcpu_id);
			vmx->loaded_vmcs->soft_vnmi_blocked = 0;
		}
	}

	if (exit_fastpath != EXIT_FASTPATH_NONE)
		return 1;

	if (exit_reason.basic >= kvm_vmx_max_exit_handlers)
		goto unexpected_vmexit;
#ifdef CONFIG_MITIGATION_RETPOLINE
	if (exit_reason.basic == EXIT_REASON_MSR_WRITE)
		return kvm_emulate_wrmsr(vcpu);
	else if (exit_reason.basic == EXIT_REASON_PREEMPTION_TIMER)
		return handle_preemption_timer(vcpu);
	else if (exit_reason.basic == EXIT_REASON_INTERRUPT_WINDOW)
		return handle_interrupt_window(vcpu);
	else if (exit_reason.basic == EXIT_REASON_EXTERNAL_INTERRUPT)
		return handle_external_interrupt(vcpu);
	else if (exit_reason.basic == EXIT_REASON_HLT)
		return kvm_emulate_halt(vcpu);
	else if (exit_reason.basic == EXIT_REASON_EPT_MISCONFIG)
		return handle_ept_misconfig(vcpu);
#endif

	exit_handler_index = array_index_nospec((u16)exit_reason.basic,
						kvm_vmx_max_exit_handlers);
	if (!kvm_vmx_exit_handlers[exit_handler_index])
		goto unexpected_vmexit;

	return kvm_vmx_exit_handlers[exit_handler_index](vcpu);

unexpected_vmexit:
	vcpu_unimpl(vcpu, "vmx: unexpected exit reason 0x%x\n",
		    exit_reason.full);
	dump_vmcs(vcpu);
#ifndef __PKVM_HYP__ /* No vcpu run page for the vcpu in pkvm hypervisor */
	vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
	vcpu->run->internal.suberror =
			KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
	vcpu->run->internal.ndata = 2;
	vcpu->run->internal.data[0] = exit_reason.full;
	vcpu->run->internal.data[1] = vcpu->arch.last_vmentry_cpu;
#endif
	return 0;
}

int vmx_handle_exit(struct kvm_vcpu *vcpu, fastpath_t exit_fastpath)
{
	int ret = __vmx_handle_exit(vcpu, exit_fastpath);

	/*
	 * Exit to user space when bus lock detected to inform that there is
	 * a bus lock in guest.
	 */
	if (to_vmx(vcpu)->exit_reason.bus_lock_detected) {
#ifndef __PKVM_HYP__ /* No vcpu run page for the vcpu in pkvm hypervisor */
		if (ret > 0)
			vcpu->run->exit_reason = KVM_EXIT_X86_BUS_LOCK;

		vcpu->run->flags |= KVM_RUN_X86_BUS_LOCK;
#endif
		return 0;
	}

	return ret;
}

static void vmx_recover_nmi_blocking(struct vcpu_vmx *vmx)
{
	u32 exit_intr_info;
	bool unblock_nmi;
	u8 vector;
	bool idtv_info_valid;

	idtv_info_valid = vmx->idt_vectoring_info & VECTORING_INFO_VALID_MASK;

	if (enable_vnmi) {
		if (vmx->loaded_vmcs->nmi_known_unmasked)
			return;

		exit_intr_info = vmx_get_intr_info(&vmx->vcpu);
		unblock_nmi = (exit_intr_info & INTR_INFO_UNBLOCK_NMI) != 0;
		vector = exit_intr_info & INTR_INFO_VECTOR_MASK;
		/*
		 * SDM 3: 27.7.1.2 (September 2008)
		 * Re-set bit "block by NMI" before VM entry if vmexit caused by
		 * a guest IRET fault.
		 * SDM 3: 23.2.2 (September 2008)
		 * Bit 12 is undefined in any of the following cases:
		 *  If the VM exit sets the valid bit in the IDT-vectoring
		 *   information field.
		 *  If the VM exit is due to a double fault.
		 */
		if ((exit_intr_info & INTR_INFO_VALID_MASK) && unblock_nmi &&
		    vector != DF_VECTOR && !idtv_info_valid)
			vmcs_set_bits(GUEST_INTERRUPTIBILITY_INFO,
				      GUEST_INTR_STATE_NMI);
		else
			vmx->loaded_vmcs->nmi_known_unmasked =
				!(vmcs_read32(GUEST_INTERRUPTIBILITY_INFO)
				  & GUEST_INTR_STATE_NMI);
	} else if (unlikely(vmx->loaded_vmcs->soft_vnmi_blocked))
#ifdef __PKVM_HYP__ /* The pkvm hypervisor doesn't support calculate time */
		return;
#else
		vmx->loaded_vmcs->vnmi_blocked_time +=
			ktime_to_ns(ktime_sub(ktime_get(),
					      vmx->loaded_vmcs->entry_time));
#endif
}

static void atomic_switch_perf_msrs(struct vcpu_vmx *vmx)
{
	int i, nr_msrs;
	struct perf_guest_switch_msr *msrs;
	struct kvm_pmu *pmu = vcpu_to_pmu(&vmx->vcpu);

	pmu->host_cross_mapped_mask = 0;
	if (pmu->pebs_enable & pmu->global_ctrl)
		intel_pmu_cross_mapped_check(pmu);

	/* Note, nr_msrs may be garbage if perf_guest_get_msrs() returns NULL. */
	msrs = vmx_perf_guest_get_msrs(&nr_msrs, (void *)pmu);
	if (!msrs)
		return;

	for (i = 0; i < nr_msrs; i++)
		if (msrs[i].host == msrs[i].guest)
			clear_atomic_switch_msr(vmx, msrs[i].msr);
		else
			add_atomic_switch_msr(vmx, msrs[i].msr, msrs[i].guest,
					msrs[i].host, false);
}

static void vmx_update_hv_timer(struct kvm_vcpu *vcpu, bool force_immediate_exit)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	u64 tscl;
	u32 delta_tsc;

	if (force_immediate_exit) {
		vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, 0);
		vmx->loaded_vmcs->hv_timer_soft_disabled = false;
	} else if (vmx->hv_deadline_tsc != -1) {
		tscl = rdtsc();
		if (vmx->hv_deadline_tsc > tscl)
			/* set_hv_timer ensures the delta fits in 32-bits */
			delta_tsc = (u32)((vmx->hv_deadline_tsc - tscl) >>
				cpu_preemption_timer_multi);
		else
			delta_tsc = 0;

		vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, delta_tsc);
		vmx->loaded_vmcs->hv_timer_soft_disabled = false;
	} else if (!vmx->loaded_vmcs->hv_timer_soft_disabled) {
		vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, -1);
		vmx->loaded_vmcs->hv_timer_soft_disabled = true;
	}
}

void noinstr vmx_update_host_rsp(struct vcpu_vmx *vmx, unsigned long host_rsp)
{
	if (unlikely(host_rsp != vmx->loaded_vmcs->host_state.rsp)) {
		vmx->loaded_vmcs->host_state.rsp = host_rsp;
		vmcs_writel(HOST_RSP, host_rsp);
	}
}

void noinstr vmx_spec_ctrl_restore_host(struct vcpu_vmx *vmx,
					unsigned int flags)
{
	u64 hostval = this_cpu_read(x86_spec_ctrl_current);

	if (!cpu_feature_enabled(X86_FEATURE_MSR_SPEC_CTRL))
		return;

	if (flags & VMX_RUN_SAVE_SPEC_CTRL)
		vmx->spec_ctrl = __rdmsr(MSR_IA32_SPEC_CTRL);

	/*
	 * If the guest/host SPEC_CTRL values differ, restore the host value.
	 *
	 * For legacy IBRS, the IBRS bit always needs to be written after
	 * transitioning from a less privileged predictor mode, regardless of
	 * whether the guest/host values differ.
	 */
	if (cpu_feature_enabled(X86_FEATURE_KERNEL_IBRS) ||
	    vmx->spec_ctrl != hostval)
		native_wrmsrl(MSR_IA32_SPEC_CTRL, hostval);

	barrier_nospec();
}

fastpath_t vmx_exit_handlers_fastpath(struct kvm_vcpu *vcpu,
				      bool force_immediate_exit)
{
	/*
	 * If L2 is active, some VMX preemption timer exits can be handled in
	 * the fastpath even, all other exits must use the slow path.
	 */
	if (is_guest_mode(vcpu) &&
	    to_vmx(vcpu)->exit_reason.basic != EXIT_REASON_PREEMPTION_TIMER)
		return EXIT_FASTPATH_NONE;

	switch (to_vmx(vcpu)->exit_reason.basic) {
#ifndef __PKVM_HYP__/* TODO: Add fastpath handling for the pkvm hypervisor */
	case EXIT_REASON_MSR_WRITE:
		return handle_fastpath_set_msr_irqoff(vcpu);
	case EXIT_REASON_HLT:
		return handle_fastpath_hlt(vcpu);
#endif
	case EXIT_REASON_PREEMPTION_TIMER:
		return handle_fastpath_preemption_timer(vcpu, force_immediate_exit);
	default:
		return EXIT_FASTPATH_NONE;
	}
}

static noinstr void vmx_vcpu_enter_exit(struct kvm_vcpu *vcpu,
					unsigned int flags)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

#ifndef __PKVM_HYP__ /* For pkvm hypervisor, leave these to host KVM */
	guest_state_enter_irqoff();

	/*
	 * L1D Flush includes CPU buffer clear to mitigate MDS, but VERW
	 * mitigation for MDS is done late in VMentry and is still
	 * executed in spite of L1D Flush. This is because an extra VERW
	 * should not matter much after the big hammer L1D Flush.
	 */
	if (static_branch_unlikely(&vmx_l1d_should_flush))
		vmx_l1d_flush(vcpu);
	else if (static_branch_unlikely(&mmio_stale_data_clear) &&
		 kvm_arch_has_assigned_device(vcpu->kvm))
		mds_clear_cpu_buffers();
#endif

	vmx_disable_fb_clear(vmx);

	if (vcpu->arch.cr2 != native_read_cr2())
		native_write_cr2(vcpu->arch.cr2);

	vmx->fail = __vmx_vcpu_run(vmx, (unsigned long *)&vcpu->arch.regs,
				   flags);

	vcpu->arch.cr2 = native_read_cr2();
	vcpu->arch.regs_avail &= ~VMX_REGS_LAZY_LOAD_SET;

	vmx->idt_vectoring_info = 0;

	vmx_enable_fb_clear(vmx);

	if (unlikely(vmx->fail)) {
		vmx->exit_reason.full = 0xdead;
		goto out;
	}

	vmx->exit_reason.full = vmcs_read32(VM_EXIT_REASON);
	if (likely(!vmx->exit_reason.failed_vmentry))
		vmx->idt_vectoring_info = vmcs_read32(IDT_VECTORING_INFO_FIELD);

#ifndef __PKVM_HYP__ /* For pkvm hypervisor, leave these to host KVM */
	if ((u16)vmx->exit_reason.basic == EXIT_REASON_EXCEPTION_NMI &&
	    is_nmi(vmx_get_intr_info(vcpu))) {
		kvm_before_interrupt(vcpu, KVM_HANDLING_NMI);
		if (cpu_feature_enabled(X86_FEATURE_FRED))
			fred_entry_from_kvm(EVENT_TYPE_NMI, NMI_VECTOR);
		else
			vmx_do_nmi_irqoff();
		kvm_after_interrupt(vcpu);
	}

out:
	guest_state_exit_irqoff();
#else
out:
	;
#endif
}

fastpath_t vmx_vcpu_run(struct kvm_vcpu *vcpu, bool force_immediate_exit)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long cr3, cr4;

#ifndef __PKVM_HYP__ /* The pkvm hypervisor doesn't support calculate time */
	/* Record the guest's net vcpu time for enforced NMI injections. */
	if (unlikely(!enable_vnmi &&
		     vmx->loaded_vmcs->soft_vnmi_blocked))
		vmx->loaded_vmcs->entry_time = ktime_get();
#endif
	/*
	 * Don't enter VMX if guest state is invalid, let the exit handler
	 * start emulation until we arrive back to a valid state.  Synthesize a
	 * consistency check VM-Exit due to invalid guest state and bail.
	 */
	if (unlikely(vmx->emulation_required)) {
		vmx->fail = 0;

		vmx->exit_reason.full = EXIT_REASON_INVALID_STATE;
		vmx->exit_reason.failed_vmentry = 1;
		kvm_register_mark_available(vcpu, VCPU_EXREG_EXIT_INFO_1);
		vmx->exit_qualification = ENTRY_FAIL_DEFAULT;
		kvm_register_mark_available(vcpu, VCPU_EXREG_EXIT_INFO_2);
		vmx->exit_intr_info = 0;
		return EXIT_FASTPATH_NONE;
	}

	trace_kvm_entry(vcpu, force_immediate_exit);

	if (vmx->ple_window_dirty) {
		vmx->ple_window_dirty = false;
		vmcs_write32(PLE_WINDOW, vmx->ple_window);
	}

	/*
	 * We did this in prepare_switch_to_guest, because it needs to
	 * be within srcu_read_lock.
	 */
	WARN_ON_ONCE(vmx->nested.need_vmcs12_to_shadow_sync);

	if (kvm_register_is_dirty(vcpu, VCPU_REGS_RSP))
		vmcs_writel(GUEST_RSP, vcpu->arch.regs[VCPU_REGS_RSP]);
	if (kvm_register_is_dirty(vcpu, VCPU_REGS_RIP))
		vmcs_writel(GUEST_RIP, vcpu->arch.regs[VCPU_REGS_RIP]);
	vcpu->arch.regs_dirty = 0;

	/*
	 * Refresh vmcs.HOST_CR3 if necessary.  This must be done immediately
	 * prior to VM-Enter, as the kernel may load a new ASID (PCID) any time
	 * it switches back to the current->mm, which can occur in KVM context
	 * when switching to a temporary mm to patch kernel code, e.g. if KVM
	 * toggles a static key while handling a VM-Exit.
	 */
#ifndef __PKVM_HYP__
	cr3 = __get_current_cr3_fast();
#else
	cr3 = __native_read_cr3();
#endif
	if (unlikely(cr3 != vmx->loaded_vmcs->host_state.cr3)) {
		vmcs_writel(HOST_CR3, cr3);
		vmx->loaded_vmcs->host_state.cr3 = cr3;
	}

#ifndef __PKVM_HYP__
	cr4 = cr4_read_shadow();
#else
	cr4 = native_read_cr4();
#endif
	if (unlikely(cr4 != vmx->loaded_vmcs->host_state.cr4)) {
		vmcs_writel(HOST_CR4, cr4);
		vmx->loaded_vmcs->host_state.cr4 = cr4;
	}

	/* When KVM_DEBUGREG_WONT_EXIT, dr6 is accessible in guest. */
	if (unlikely(vcpu->arch.switch_db_regs & KVM_DEBUGREG_WONT_EXIT))
		set_debugreg(vcpu->arch.dr6, 6);

	/* When single-stepping over STI and MOV SS, we must clear the
	 * corresponding interruptibility bits in the guest state. Otherwise
	 * vmentry fails as it then expects bit 14 (BS) in pending debug
	 * exceptions being set, but that's not correct for the guest debugging
	 * case. */
	if (vcpu->guest_debug & KVM_GUESTDBG_SINGLESTEP)
		vmx_set_interrupt_shadow(vcpu, 0);

	kvm_load_guest_xsave_state(vcpu);

	pt_guest_enter(vmx);

	atomic_switch_perf_msrs(vmx);
	if (intel_pmu_lbr_is_enabled(vcpu))
		vmx_passthrough_lbr_msrs(vcpu);

	if (enable_preemption_timer)
		vmx_update_hv_timer(vcpu, force_immediate_exit);
#ifndef __PKVM_HYP__ /* For pkvm hypervisor, leave these to host KVM */
	else if (force_immediate_exit)
		smp_send_reschedule(vcpu->cpu);

	kvm_wait_lapic_expire(vcpu);
#endif

	/* The actual VMENTER/EXIT is in the .noinstr.text section. */
	vmx_vcpu_enter_exit(vcpu, __vmx_vcpu_run_flags(vmx));

#ifndef __PKVM_HYP__
	/* All fields are clean at this point */
	if (kvm_is_using_evmcs()) {
		current_evmcs->hv_clean_fields |=
			HV_VMX_ENLIGHTENED_CLEAN_FIELD_ALL;

		current_evmcs->hv_vp_id = kvm_hv_get_vpindex(vcpu);
	}
#endif

	/* MSR_IA32_DEBUGCTLMSR is zeroed on vmexit. Restore it if needed */
	if (vcpu->arch.host_debugctl)
		update_debugctlmsr(vcpu->arch.host_debugctl);

#ifndef CONFIG_X86_64
	/*
	 * The sysexit path does not restore ds/es, so we must set them to
	 * a reasonable value ourselves.
	 *
	 * We can't defer this to vmx_prepare_switch_to_host() since that
	 * function may be executed in interrupt context, which saves and
	 * restore segments around it, nullifying its effect.
	 */
	loadsegment(ds, __USER_DS);
	loadsegment(es, __USER_DS);
#endif

	pt_guest_exit(vmx);

	kvm_load_host_xsave_state(vcpu);

	if (is_guest_mode(vcpu)) {
		/*
		 * Track VMLAUNCH/VMRESUME that have made past guest state
		 * checking.
		 */
		if (vmx->nested.nested_run_pending &&
		    !vmx->exit_reason.failed_vmentry)
			++vcpu->stat.nested_run;

		vmx->nested.nested_run_pending = 0;
	}

	if (unlikely(vmx->fail))
		return EXIT_FASTPATH_NONE;

#ifndef __PKVM_HYP__ /* For pkvm hypervisor, leave these to host KVM */
	if (unlikely((u16)vmx->exit_reason.basic == EXIT_REASON_MCE_DURING_VMENTRY))
		kvm_machine_check();

#endif
	trace_kvm_exit(vcpu, KVM_ISA_VMX);

	if (unlikely(vmx->exit_reason.failed_vmentry))
		return EXIT_FASTPATH_NONE;

	vmx->loaded_vmcs->launched = 1;

	vmx_recover_nmi_blocking(vmx);
#ifndef __PKVM_HYP__ /* FIXME: implement in pkvm hypervisor */
	vmx_complete_interrupts(vmx);
#endif

	return vmx_exit_handlers_fastpath(vcpu, force_immediate_exit);
}

void vmx_vcpu_free(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (enable_pml)
		vmx_destroy_pml_buffer(vmx);
	free_vpid(vmx->vpid);
	nested_vmx_free_vcpu(vcpu);
	free_loaded_vmcs(vmx->loaded_vmcs);
	vmx_destroy_ve(vmx);

#ifdef __PKVM_HYP__
	/*
	 * FIXME: Teardown shadow vcpu as it is initialized when creating vcpu.
	 * Should revisit when PV method is ready.
	 */
	pkvm_teardown_shadow_vcpu(vcpu);
#endif
}

int vmx_vcpu_create(struct kvm_vcpu *vcpu)
{
#ifdef __PKVM_HYP__
	struct vcpu_vmx *shared_vmx;
#endif
	struct vmx_uret_msr *tsx_ctrl;
	struct vcpu_vmx *vmx;
#ifndef __PKVM_HYP__
	struct page *page;
#endif
	int i, err;

#ifdef __PKVM_HYP__
	BUILD_BUG_ON(offsetof(struct pkvm_vcpu_vmx, vmx) != 0);
#endif
	BUILD_BUG_ON(offsetof(struct vcpu_vmx, vcpu) != 0);
	vmx = to_vmx(vcpu);

#ifdef __PKVM_HYP__
	shared_vmx = to_vmx(to_pkvm_vcpu(vcpu)->shared_vcpu);
#endif

	INIT_LIST_HEAD(&vmx->pi_wakeup_list);

	err = -ENOMEM;

	shared_vmx->vpid = vmx->vpid = allocate_vpid();

	/*
	 * If PML is turned on, failure on enabling PML just results in failure
	 * of creating the vcpu, therefore we can simplify PML logic (by
	 * avoiding dealing with cases, such as enabling PML partially on vcpus
	 * for the guest), etc.
	 */
	if (enable_pml) {
#ifdef __PKVM_HYP__
		vmx->pml_pg = kern_pkvm_va(shared_vmx->pml_pg);
		/*
		 * TODO: donate pml_pg. Cannot do this right now as the linux
		 * host may still need to access pml page.
		 */
		if (!vmx->pml_pg/*||
			__pkvm_host_donate_hyp(__pkvm_pa(vmx->pml_pg), PAGE_SIZE)*/)
			goto free_vpid;
#else
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page)
			goto free_vpid;

		vmx->pml_pg = page_to_virt(page);
#endif
	}

	for (i = 0; i < kvm_nr_uret_msrs; ++i)
		shared_vmx->guest_uret_msrs[i].mask = vmx->guest_uret_msrs[i].mask = -1ull;
	if (boot_cpu_has(X86_FEATURE_RTM)) {
		/*
		 * TSX_CTRL_CPUID_CLEAR is handled in the CPUID interception.
		 * Keep the host value unchanged to avoid changing CPUID bits
		 * under the host kernel's feet.
		 */
		tsx_ctrl = vmx_find_uret_msr(vmx, MSR_IA32_TSX_CTRL);
		if (tsx_ctrl)
			tsx_ctrl->mask = ~(u64)TSX_CTRL_CPUID_CLEAR;
	}

#ifdef __PKVM_HYP__
	vmx->vmcs01.vmcs = kern_pkvm_va(shared_vmx->vmcs01.vmcs);
	vmx->vmcs01.msr_bitmap = kern_pkvm_va(shared_vmx->vmcs01.msr_bitmap);
#endif
	err = alloc_loaded_vmcs(&vmx->vmcs01);
	if (err < 0)
		goto free_pml;

#ifndef __PKVM_HYP__
	/*
	 * Use Hyper-V 'Enlightened MSR Bitmap' feature when KVM runs as a
	 * nested (L1) hypervisor and Hyper-V in L0 supports it. Enable the
	 * feature only for vmcs01, KVM currently isn't equipped to realize any
	 * performance benefits from enabling it for vmcs02.
	 */
	if (kvm_is_using_evmcs() &&
	    (ms_hyperv.nested_features & HV_X64_NESTED_MSR_BITMAP)) {
		struct hv_enlightened_vmcs *evmcs = (void *)vmx->vmcs01.vmcs;

		evmcs->hv_enlightenments_control.msr_bitmap = 1;
	}
#endif

	/* The MSR bitmap starts with all ones */
	bitmap_fill(vmx->shadow_msr_intercept.read, MAX_POSSIBLE_PASSTHROUGH_MSRS);
	bitmap_fill(vmx->shadow_msr_intercept.write, MAX_POSSIBLE_PASSTHROUGH_MSRS);

	vmx_disable_intercept_for_msr(vcpu, MSR_IA32_TSC, MSR_TYPE_R);
#ifdef CONFIG_X86_64
	vmx_disable_intercept_for_msr(vcpu, MSR_FS_BASE, MSR_TYPE_RW);
	vmx_disable_intercept_for_msr(vcpu, MSR_GS_BASE, MSR_TYPE_RW);
	vmx_disable_intercept_for_msr(vcpu, MSR_KERNEL_GS_BASE, MSR_TYPE_RW);
#endif
	vmx_disable_intercept_for_msr(vcpu, MSR_IA32_SYSENTER_CS, MSR_TYPE_RW);
	vmx_disable_intercept_for_msr(vcpu, MSR_IA32_SYSENTER_ESP, MSR_TYPE_RW);
	vmx_disable_intercept_for_msr(vcpu, MSR_IA32_SYSENTER_EIP, MSR_TYPE_RW);
	if (kvm_cstate_in_guest(vcpu->kvm)) {
		vmx_disable_intercept_for_msr(vcpu, MSR_CORE_C1_RES, MSR_TYPE_R);
		vmx_disable_intercept_for_msr(vcpu, MSR_CORE_C3_RESIDENCY, MSR_TYPE_R);
		vmx_disable_intercept_for_msr(vcpu, MSR_CORE_C6_RESIDENCY, MSR_TYPE_R);
		vmx_disable_intercept_for_msr(vcpu, MSR_CORE_C7_RESIDENCY, MSR_TYPE_R);
	}

	vmx->loaded_vmcs = &vmx->vmcs01;

	/*
	 * For the pkvm hypervisor, the apic access page should be allocated by
	 * the host.
	 */
#ifndef __PKVM_HYP__
	if (cpu_need_virtualize_apic_accesses(vcpu)) {
		err = kvm_alloc_apic_access_page(vcpu->kvm);
		if (err)
			goto free_vmcs;
	}
#endif
	if (enable_ept && !enable_unrestricted_guest) {
#ifdef __PKVM_HYP__
		/* The pkvm hypervisor requires unrestricted_guest */
		goto free_vmcs;
#else
		err = init_rmode_identity_map(vcpu->kvm);
		if (err)
			goto free_vmcs;
#endif
	}

	err = -ENOMEM;
	if (vmcs_config.cpu_based_2nd_exec_ctrl & SECONDARY_EXEC_EPT_VIOLATION_VE) {
		BUILD_BUG_ON(sizeof(*vmx->ve_info) > PAGE_SIZE);
#ifdef __PKVM_HYP__
		vmx->ve_info = kern_pkvm_va(shared_vmx->ve_info);
		/*
		 * TODO: donate ve_info. Cannot do this right now as the linux
		 * host may still need to access ve_info page.
		 */
		if (!vmx->ve_info /*|| __pkvm_host_donate_hyp(__pkvm_pa(vmx->ve_info), PAGE_SIZE)*/)
			goto free_vmcs;
#else
		/* ve_info must be page aligned. */
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page)
			goto free_vmcs;

		vmx->ve_info = page_to_virt(page);
#endif
	}

	/* TODO: support ipiv in the pkvm hypervisor */
#ifndef __PKVM_HYP__
	if (vmx_can_use_ipiv(vcpu))
		WRITE_ONCE(to_kvm_vmx(vcpu->kvm)->pid_table[vcpu->vcpu_id],
			   __pa(&vmx->pi_desc) | PID_TABLE_ENTRY_VALID);
#endif

#ifdef __PKVM_HYP__
	err = pkvm_init_shadow_vcpu(vcpu);
	if (err)
		goto free_ve;
#endif

	return 0;

#ifdef __PKVM_HYP__
free_ve:
	vmx_destroy_ve(vmx);
#endif
free_vmcs:
	free_loaded_vmcs(vmx->loaded_vmcs);
free_pml:
	vmx_destroy_pml_buffer(vmx);
free_vpid:
	free_vpid(vmx->vpid);
	return err;
}

int vmx_vm_init(struct kvm *kvm)
{
	if (!ple_gap)
		kvm->arch.pause_in_guest = true;

#ifdef __PKVM_HYP__
	BUILD_BUG_ON(offsetof(struct pkvm_vm_vmx, kvm_vmx) != 0);
	BUILD_BUG_ON(offsetof(struct kvm_vmx, kvm) != 0);

	/*
	 * FIXME: Initialize shadow VM for using VMX emulation method.
	 * Should revisit when PV method is ready.
	 */
	return pkvm_init_shadow_vm(kvm);
#else
	if (boot_cpu_has(X86_BUG_L1TF) && enable_ept) {
		switch (l1tf_mitigation) {
		case L1TF_MITIGATION_OFF:
		case L1TF_MITIGATION_FLUSH_NOWARN:
			/* 'I explicitly don't care' is set */
			break;
		case L1TF_MITIGATION_FLUSH:
		case L1TF_MITIGATION_FLUSH_NOSMT:
		case L1TF_MITIGATION_FULL:
			/*
			 * Warn upon starting the first VM in a potentially
			 * insecure environment.
			 */
			if (sched_smt_active())
				pr_warn_once(L1TF_MSG_SMT);
			if (l1tf_vmx_mitigation == VMENTER_L1D_FLUSH_NEVER)
				pr_warn_once(L1TF_MSG_L1D);
			break;
		case L1TF_MITIGATION_FULL_FORCE:
			/* Flush is enforced */
			break;
		}
	}

	return 0;
#endif
}

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

void vmx_vm_destroy(struct kvm *kvm)
{
#ifdef __PKVM_HYP__
	/*
	 * FIXME: Teardown shadow vm as it is initialized when doing
	 * vm init. Should revisit when PV method is ready.
	 */
	pkvm_teardown_shadow_vm(kvm);

	/*
	 * TODO: Support teardown the pid_table in the pkvm hypervisor when ipiv
	 * is supported.
	 */
#else
	struct kvm_vmx *kvm_vmx = to_kvm_vmx(kvm);

	free_pages((unsigned long)kvm_vmx->pid_table, vmx_get_pid_table_order(kvm));
#endif
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
#ifdef __PKVM_HYP__
		return -EINVAL;
#else
		enable_ept = 0;
#endif

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
#ifdef __PKVM_HYP__
		return -EINVAL;
#else
		enable_vnmi = 0;
#endif

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
#ifdef __PKVM_HYP__
		/*
		 * The pkvm hypervisor requires to use the preemption timer
		 * to handle the force_immediate_exit for the vcpu run
		 */
		return -EINVAL;
#else
		vt_x86_ops.set_hv_timer = NULL;
		vt_x86_ops.cancel_hv_timer = NULL;
#endif
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
static void vmx_switch_to_guest_vcpu(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int cpu = raw_smp_processor_id();
	struct vcpu_vmx *shared_vmx;

	shared_vmx = to_vmx(to_pkvm_vcpu(vcpu)->shared_vcpu);
	/*
	 * Currently the PV method is not fully functional and the VMX working
	 * model is in a mixed way. Some meta data and the guest registers in
	 * the kvm_vcpu structure are used by both side so should to keep them
	 * in sync between the host and the pkvm hypervisor as a temporary
	 * hack.
	 */
	memcpy(&vmx->vmcs01.controls_shadow, &shared_vmx->vmcs01.controls_shadow,
	       sizeof(struct vmcs_controls_shadow));

	memcpy(&vmx->guest_uret_msrs, &shared_vmx->guest_uret_msrs,
	       sizeof(struct vmx_uret_msr) * MAX_NR_USER_RETURN_MSRS);

	vmx->hv_deadline_tsc = shared_vmx->hv_deadline_tsc;
	vmx->msr_ia32_feature_control_valid_bits =
			shared_vmx->msr_ia32_feature_control_valid_bits;

	vmx->vcpu.arch.regs_dirty |= shared_vmx->vcpu.arch.regs_dirty;
	memcpy(vmx->vcpu.arch.regs, shared_vmx->vcpu.arch.regs,
	       sizeof(unsigned long) * NR_VCPU_REGS);
	vmx->rflags = shared_vmx->rflags;
	vmx->vcpu.arch.cr0 = shared_vmx->vcpu.arch.cr0;
	vmx->vcpu.arch.cr2 = shared_vmx->vcpu.arch.cr2;
	vmx->vcpu.arch.cr3 = shared_vmx->vcpu.arch.cr3;
	vmx->vcpu.arch.cr4 = shared_vmx->vcpu.arch.cr4;
	vmx->vcpu.arch.efer = shared_vmx->vcpu.arch.efer;
	vmx->vcpu.arch.pat = shared_vmx->vcpu.arch.pat;
	vmx->vcpu.arch.xcr0 = shared_vmx->vcpu.arch.xcr0;

	/* Guest vcpu is not loaded, no need to switch */
	if (vmx->loaded_vmcs->cpu == -1)
		return;

	if (WARN_ON_ONCE(vmx->loaded_vmcs->cpu != cpu))
		return;

	/*
	 * The VMX emulate-based method is still in use. Make sure
	 * the guest VMCS shadow indicator is cleared before loading it.
	 */
	vmx->loaded_vmcs->vmcs->hdr.shadow_vmcs = 0;
	/* Load guest vmcs */
	vmcs_load(vmx->loaded_vmcs->vmcs);

	/* Sync the vmcs12 to vmcs02 to satisfy the VMX emulate-based method */
	pkvm_sync_emulated_fields_vmcs12to02(vcpu);
}

static void vmx_switch_to_host_vcpu(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int cpu = raw_smp_processor_id();
	struct vcpu_vmx *shared_vmx;

	shared_vmx = to_vmx(to_pkvm_vcpu(vcpu)->shared_vcpu);
	/* Similar to data sync in the vmx_switch_to_guest_vcpu */
	memcpy(&shared_vmx->vmcs01.controls_shadow, &vmx->vmcs01.controls_shadow,
	       sizeof(struct vmcs_controls_shadow));

	memcpy(&shared_vmx->guest_uret_msrs, &vmx->guest_uret_msrs,
	       sizeof(struct vmx_uret_msr) * MAX_NR_USER_RETURN_MSRS);

	shared_vmx->hv_deadline_tsc = vmx->hv_deadline_tsc;
	shared_vmx->msr_ia32_feature_control_valid_bits =
			vmx->msr_ia32_feature_control_valid_bits;

	shared_vmx->vcpu.arch.regs_dirty |= vmx->vcpu.arch.regs_dirty;
	memcpy(shared_vmx->vcpu.arch.regs, vmx->vcpu.arch.regs,
	       sizeof(unsigned long) * NR_VCPU_REGS);
	shared_vmx->vcpu.arch.regs[VCPU_REGS_RIP] = kvm_rip_read(vcpu);
	shared_vmx->vcpu.arch.regs[VCPU_REGS_RSP] = kvm_rsp_read(vcpu);
	shared_vmx->rflags = vmx_get_rflags(vcpu);
	shared_vmx->vcpu.arch.cr0 = kvm_read_cr0(vcpu);
	shared_vmx->vcpu.arch.cr2 = vmx->vcpu.arch.cr2;
	shared_vmx->vcpu.arch.cr3 = kvm_read_cr3(vcpu);
	shared_vmx->vcpu.arch.cr4 = kvm_read_cr4(vcpu);
	shared_vmx->vcpu.arch.regs_avail |= (1 << VCPU_REGS_RIP) |
					    (1 << VCPU_REGS_RSP) |
					    (1 << VCPU_EXREG_RFLAGS) |
					    (1 << VCPU_EXREG_CR0) |
					    (1 << VCPU_EXREG_CR3) |
					    (1 << VCPU_EXREG_CR4);
	shared_vmx->vcpu.arch.efer = vmx->vcpu.arch.efer;
	shared_vmx->vcpu.arch.pat = vmx->vcpu.arch.pat;
	shared_vmx->vcpu.arch.xcr0 = vmx->vcpu.arch.xcr0;

	shared_vmx->fail = vmx->fail;
	shared_vmx->exit_reason.full = vmx->exit_reason.full;
	shared_vmx->idt_vectoring_info = vmx->idt_vectoring_info;
	vmx_get_intr_info(&shared_vmx->vcpu);
	vmx_get_exit_qual(&shared_vmx->vcpu);

	if (vmx->loaded_vmcs->cpu == cpu) {
		vmcs_load(vmx->loaded_vmcs->vmcs);
		/*
		 * In pair with vmx_switch_to_guest_vcpu, sync vmcs02 to vmcs12
		 * to make sure the right value can be write back to the vmcs02
		 * when calls pkvm_sync_emulated_fields_vmcs12to02.
		 */
		pkvm_sync_emulated_fields_vmcs02to12(vcpu);

		/*
		 * The VMX emulate-based method is still in use, which requires
		 * to launch the guest VMCS and set the shadow indicator.
		 */
		vmcs_clear(vmx->loaded_vmcs->vmcs);
		vmx->loaded_vmcs->launched = 0;
		vmx->loaded_vmcs->vmcs->hdr.shadow_vmcs = 1;
	}

	/* Load host vmcs */
	vmcs_load(to_vmx(this_cpu_read(host_vcpu))->vmcs01.vmcs);
}

struct kvm_x86_ops vt_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.check_processor_compatibility = vmx_check_processor_compat,

	.enable_virtualization_cpu = vmx_enable_virtualization_cpu,
	.disable_virtualization_cpu = vmx_disable_virtualization_cpu,

	.vm_init = vmx_vm_init,
	.vm_destroy = vmx_vm_destroy,

	.vcpu_create = vmx_vcpu_create,
	.vcpu_free = vmx_vcpu_free,

	.prepare_switch_to_guest = vmx_prepare_switch_to_guest,
	.prepare_switch_to_host = vmx_prepare_switch_to_host,
	.vcpu_load = vmx_vcpu_load,
	.vcpu_put = vmx_vcpu_put,

	.cache_reg = vmx_cache_reg,
	.get_rflags = vmx_get_rflags,

	.flush_tlb_all = vmx_flush_tlb_all,
	.flush_tlb_current = vmx_flush_tlb_current,

	.vcpu_pre_run = vmx_vcpu_pre_run,
	.vcpu_run = vmx_vcpu_run,
	.handle_exit = vmx_handle_exit,
	.skip_emulated_instruction = vmx_skip_emulated_instruction,
};

struct kvm_x86_init_ops vt_init_ops __initdata = {
	.hardware_setup = vmx_hardware_setup,
	.runtime_ops = &vt_x86_ops,
};

static struct pkvm_x86_ops pkvm_vt_x86_ops = {
	.switch_to_guest_vcpu = vmx_switch_to_guest_vcpu,
	.switch_to_host_vcpu = vmx_switch_to_host_vcpu,
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

#ifdef CONFIG_PKVM_INTEL_DEBUG
	dump_invalid_vmcs = true;
#endif

	for_each_possible_cpu(cpu)
		INIT_LIST_HEAD(&per_cpu(loaded_vmcss_on_cpu, cpu));

	pkvm_vm_sz = sizeof(struct pkvm_vm) + sizeof(struct pkvm_vm_vmx);
	pkvm_vcpu_sz = sizeof(struct pkvm_vcpu) + sizeof(struct pkvm_vcpu_vmx);

	pkvm_x86_ops_init(&pkvm_vt_x86_ops);

	return kvm_x86_vendor_init(&vt_init_ops);
}
#endif
