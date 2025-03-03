// SPDX-License-Identifier: GPL-2.0
#include <x86.h>
#include <asm/fpu/xcr.h>

#ifdef __PKVM_HYP__
#undef module_param_named
#define module_param_named(...)
#endif

/*
 * Note, kvm_caps fields should *never* have default values, all fields must be
 * recomputed from scratch during vendor module load, e.g. to account for a
 * vendor module being reloaded with different module parameters.
 */
struct kvm_caps kvm_caps __read_mostly;
EXPORT_SYMBOL_GPL(kvm_caps);

struct kvm_host_values kvm_host __read_mostly;
EXPORT_SYMBOL_GPL(kvm_host);

/* EFER defaults:
 * - enable syscall per default because its emulated by KVM
 * - enable LME and LMA per default on 64 bit KVM
 */
#ifdef CONFIG_X86_64
static
u64 __read_mostly efer_reserved_bits = ~((u64)(EFER_SCE | EFER_LME | EFER_LMA));
#else
static u64 __read_mostly efer_reserved_bits = ~((u64)EFER_SCE);
#endif

struct kvm_x86_ops kvm_x86_ops __read_mostly;

/* Enable/disable PMU virtualization */
bool __read_mostly enable_pmu = true;
EXPORT_SYMBOL_GPL(enable_pmu);
module_param(enable_pmu, bool, 0444);

/*
 * Restoring the host value for MSRs that are only consumed when running in
 * usermode, e.g. SYSCALL MSRs and TSC_AUX, can be deferred until the CPU
 * returns to userspace, i.e. the kernel can run with the guest's value.
 */
#define KVM_MAX_NR_USER_RETURN_MSRS 16

u32 __read_mostly kvm_nr_uret_msrs;
EXPORT_SYMBOL_GPL(kvm_nr_uret_msrs);
static u32 __read_mostly kvm_uret_msrs_list[KVM_MAX_NR_USER_RETURN_MSRS];

#define KVM_SUPPORTED_XCR0     (XFEATURE_MASK_FP | XFEATURE_MASK_SSE \
				| XFEATURE_MASK_YMM | XFEATURE_MASK_BNDREGS \
				| XFEATURE_MASK_BNDCSR | XFEATURE_MASK_AVX512 \
				| XFEATURE_MASK_PKRU | XFEATURE_MASK_XTILE)

bool __read_mostly enable_apicv = true;
EXPORT_SYMBOL_GPL(enable_apicv);

static int kvm_probe_user_return_msr(u32 msr)
{
	u64 val;
	int ret;

	preempt_disable();
	ret = rdmsrl_safe(msr, &val);
	if (ret)
		goto out;
	ret = wrmsrl_safe(msr, val);
out:
	preempt_enable();
	return ret;
}

int kvm_add_user_return_msr(u32 msr)
{
	BUG_ON(kvm_nr_uret_msrs >= KVM_MAX_NR_USER_RETURN_MSRS);

	if (kvm_probe_user_return_msr(msr))
		return -1;

	kvm_uret_msrs_list[kvm_nr_uret_msrs] = msr;
	return kvm_nr_uret_msrs++;
}
EXPORT_SYMBOL_GPL(kvm_add_user_return_msr);

int kvm_find_user_return_msr(u32 msr)
{
	int i;

	for (i = 0; i < kvm_nr_uret_msrs; ++i) {
		if (kvm_uret_msrs_list[i] == msr)
			return i;
	}
	return -1;
}
EXPORT_SYMBOL_GPL(kvm_find_user_return_msr);

noinstr void kvm_spurious_fault(void)
{
#ifndef __PKVM_HYP__
	/* Fault while not rebooting.  We want the trace. */
	BUG_ON(!kvm_rebooting);
#endif
}
EXPORT_SYMBOL_GPL(kvm_spurious_fault);

void kvm_enable_efer_bits(u64 mask)
{
       efer_reserved_bits &= ~mask;
}
EXPORT_SYMBOL_GPL(kvm_enable_efer_bits);

int kvm_x86_vendor_init(struct kvm_x86_init_ops *ops)
{
#ifdef __PKVM_HYP__
	int r;

	kvm_caps.max_guest_tsc_khz = tsc_khz;
	kvm_caps.supported_vm_types = BIT(KVM_X86_DEFAULT_VM);
	kvm_caps.supported_mce_cap = MCG_CTL_P | MCG_SER_P;

	if (boot_cpu_has(X86_FEATURE_XSAVE)) {
		kvm_host.xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
		kvm_caps.supported_xcr0 = kvm_host.xcr0 & KVM_SUPPORTED_XCR0;
	}

	rdmsrl_safe(MSR_EFER, &kvm_host.efer);

	if (boot_cpu_has(X86_FEATURE_XSAVES))
		rdmsrl(MSR_IA32_XSS, kvm_host.xss);

	if (boot_cpu_has(X86_FEATURE_ARCH_CAPABILITIES))
		rdmsrl(MSR_IA32_ARCH_CAPABILITIES, kvm_host.arch_capabilities);

	memcpy(&kvm_x86_ops, ops->runtime_ops, sizeof(kvm_x86_ops));

	r = ops->hardware_setup();
	if (r != 0)
		return r;

	return 0;
#else
	u64 host_pat;
	int r, cpu;

	guard(mutex)(&vendor_module_lock);

	if (kvm_x86_ops.enable_virtualization_cpu) {
		pr_err("already loaded vendor module '%s'\n", kvm_x86_ops.name);
		return -EEXIST;
	}

	if (ops->pkvm_init && ops->pkvm_init()) {
		pr_err_ratelimited("kvm: pkvm init fail\n");
		return -EOPNOTSUPP;
	}

	/*
	 * KVM explicitly assumes that the guest has an FPU and
	 * FXSAVE/FXRSTOR. For example, the KVM_GET_FPU explicitly casts the
	 * vCPU's FPU state as a fxregs_state struct.
	 */
	if (!boot_cpu_has(X86_FEATURE_FPU) || !boot_cpu_has(X86_FEATURE_FXSR)) {
		pr_err("inadequate fpu\n");
		return -EOPNOTSUPP;
	}

	if (IS_ENABLED(CONFIG_PREEMPT_RT) && !boot_cpu_has(X86_FEATURE_CONSTANT_TSC)) {
		pr_err("RT requires X86_FEATURE_CONSTANT_TSC\n");
		return -EOPNOTSUPP;
	}

	/*
	 * KVM assumes that PAT entry '0' encodes WB memtype and simply zeroes
	 * the PAT bits in SPTEs.  Bail if PAT[0] is programmed to something
	 * other than WB.  Note, EPT doesn't utilize the PAT, but don't bother
	 * with an exception.  PAT[0] is set to WB on RESET and also by the
	 * kernel, i.e. failure indicates a kernel bug or broken firmware.
	 */
	if (rdmsrl_safe(MSR_IA32_CR_PAT, &host_pat) ||
	    (host_pat & GENMASK(2, 0)) != 6) {
		pr_err("host PAT[0] is not WB\n");
		return -EIO;
	}

	memset(&kvm_caps, 0, sizeof(kvm_caps));

	x86_emulator_cache = kvm_alloc_emulator_cache();
	if (!x86_emulator_cache) {
		pr_err("failed to allocate cache for x86 emulator\n");
		return -ENOMEM;
	}

	user_return_msrs = alloc_percpu(struct kvm_user_return_msrs);
	if (!user_return_msrs) {
		pr_err("failed to allocate percpu kvm_user_return_msrs\n");
		r = -ENOMEM;
		goto out_free_x86_emulator_cache;
	}
	kvm_nr_uret_msrs = 0;

	r = kvm_mmu_vendor_module_init();
	if (r)
		goto out_free_percpu;

	kvm_caps.supported_vm_types = BIT(KVM_X86_DEFAULT_VM);
	kvm_caps.supported_mce_cap = MCG_CTL_P | MCG_SER_P;

	if (boot_cpu_has(X86_FEATURE_XSAVE)) {
		kvm_host.xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
		kvm_caps.supported_xcr0 = kvm_host.xcr0 & KVM_SUPPORTED_XCR0;
	}

	rdmsrl_safe(MSR_EFER, &kvm_host.efer);

	if (boot_cpu_has(X86_FEATURE_XSAVES))
		rdmsrl(MSR_IA32_XSS, kvm_host.xss);

	kvm_init_pmu_capability(ops->pmu_ops);

	if (boot_cpu_has(X86_FEATURE_ARCH_CAPABILITIES))
		rdmsrl(MSR_IA32_ARCH_CAPABILITIES, kvm_host.arch_capabilities);

	r = ops->hardware_setup();
	if (r != 0)
		goto out_mmu_exit;

	kvm_ops_update(ops);

	for_each_online_cpu(cpu) {
		smp_call_function_single(cpu, kvm_x86_check_cpu_compat, &r, 1);
		if (r < 0)
			goto out_unwind_ops;
	}

	/*
	 * Point of no return!  DO NOT add error paths below this point unless
	 * absolutely necessary, as most operations from this point forward
	 * require unwinding.
	 */
	kvm_timer_init();

	if (pi_inject_timer == -1)
		pi_inject_timer = housekeeping_enabled(HK_TYPE_TIMER);
#ifdef CONFIG_X86_64
	pvclock_gtod_register_notifier(&pvclock_gtod_notifier);

	if (hypervisor_is_type(X86_HYPER_MS_HYPERV))
		set_hv_tscchange_cb(kvm_hyperv_tsc_notifier);
#endif

	kvm_register_perf_callbacks(ops->handle_intel_pt_intr);

	if (IS_ENABLED(CONFIG_KVM_SW_PROTECTED_VM) && tdp_mmu_enabled)
		kvm_caps.supported_vm_types |= BIT(KVM_X86_SW_PROTECTED_VM);

#ifdef CONFIG_PKVM_INTEL
	if (enable_pkvm && tdp_mmu_enabled)
		kvm_caps.supported_vm_types |= BIT(KVM_X86_PKVM_PROTECTED_VM);
#endif

	if (!kvm_cpu_cap_has(X86_FEATURE_XSAVES))
		kvm_caps.supported_xss = 0;

#define __kvm_cpu_cap_has(UNUSED_, f) kvm_cpu_cap_has(f)
	cr4_reserved_bits = __cr4_reserved_bits(__kvm_cpu_cap_has, UNUSED_);
#undef __kvm_cpu_cap_has

	if (kvm_caps.has_tsc_control) {
		/*
		 * Make sure the user can only configure tsc_khz values that
		 * fit into a signed integer.
		 * A min value is not calculated because it will always
		 * be 1 on all machines.
		 */
		u64 max = min(0x7fffffffULL,
			      __scale_tsc(kvm_caps.max_tsc_scaling_ratio, tsc_khz));
		kvm_caps.max_guest_tsc_khz = max;
	}
	kvm_caps.default_tsc_scaling_ratio = 1ULL << kvm_caps.tsc_scaling_ratio_frac_bits;
	kvm_init_msr_lists();
	return 0;

out_unwind_ops:
	kvm_x86_ops.enable_virtualization_cpu = NULL;
	kvm_x86_call(hardware_unsetup)();
out_mmu_exit:
	kvm_mmu_vendor_module_exit();
out_free_percpu:
	free_percpu(user_return_msrs);
out_free_x86_emulator_cache:
	kmem_cache_destroy(x86_emulator_cache);
	return r;
#endif
}
EXPORT_SYMBOL_GPL(kvm_x86_vendor_init);
