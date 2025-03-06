// SPDX-License-Identifier: GPL-2.0
#include "x86.h"
#include <asm/debugreg.h>
#include <asm/fpu/xcr.h>
#include <cpuid.h>

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

void kvm_load_guest_xsave_state(struct kvm_vcpu *vcpu)
{
#ifndef __PKVM_HYP__
	if (vcpu->arch.guest_state_protected)
		return;
#endif

	if (kvm_is_cr4_bit_set(vcpu, X86_CR4_OSXSAVE)) {

		if (vcpu->arch.xcr0 != kvm_host.xcr0)
			xsetbv(XCR_XFEATURE_ENABLED_MASK, vcpu->arch.xcr0);

		if (guest_can_use(vcpu, X86_FEATURE_XSAVES) &&
		    vcpu->arch.ia32_xss != kvm_host.xss)
			wrmsrl(MSR_IA32_XSS, vcpu->arch.ia32_xss);
	}

	if (cpu_feature_enabled(X86_FEATURE_PKU) &&
	    vcpu->arch.pkru != vcpu->arch.host_pkru &&
	    ((vcpu->arch.xcr0 & XFEATURE_MASK_PKRU) ||
	     kvm_is_cr4_bit_set(vcpu, X86_CR4_PKE)))
		write_pkru(vcpu->arch.pkru);
}
EXPORT_SYMBOL_GPL(kvm_load_guest_xsave_state);

void kvm_load_host_xsave_state(struct kvm_vcpu *vcpu)
{
#ifndef __PKVM_HYP__
	if (vcpu->arch.guest_state_protected)
		return;
#endif

	if (cpu_feature_enabled(X86_FEATURE_PKU) &&
	    ((vcpu->arch.xcr0 & XFEATURE_MASK_PKRU) ||
	     kvm_is_cr4_bit_set(vcpu, X86_CR4_PKE))) {
		vcpu->arch.pkru = rdpkru();
		if (vcpu->arch.pkru != vcpu->arch.host_pkru)
			write_pkru(vcpu->arch.host_pkru);
	}

	if (kvm_is_cr4_bit_set(vcpu, X86_CR4_OSXSAVE)) {

		if (vcpu->arch.xcr0 != kvm_host.xcr0)
			xsetbv(XCR_XFEATURE_ENABLED_MASK, kvm_host.xcr0);

		if (guest_can_use(vcpu, X86_FEATURE_XSAVES) &&
		    vcpu->arch.ia32_xss != kvm_host.xss)
			wrmsrl(MSR_IA32_XSS, kvm_host.xss);
	}
}
EXPORT_SYMBOL_GPL(kvm_load_host_xsave_state);

int kvm_emulate_xsetbv(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_xsetbv);

int kvm_emulate_rdpmc(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_rdpmc);

void kvm_enable_efer_bits(u64 mask)
{
       efer_reserved_bits &= ~mask;
}
EXPORT_SYMBOL_GPL(kvm_enable_efer_bits);

bool kvm_msr_allowed(struct kvm_vcpu *vcpu, u32 index, u32 type)
{
#ifdef __PKVM_HYP__
	/* TODO: Support MSR filter */
	return true;
#else
	struct kvm_x86_msr_filter *msr_filter;
	struct msr_bitmap_range *ranges;
	struct kvm *kvm = vcpu->kvm;
	bool allowed;
	int idx;
	u32 i;

	/* x2APIC MSRs do not support filtering. */
	if (index >= 0x800 && index <= 0x8ff)
		return true;

	idx = srcu_read_lock(&kvm->srcu);

	msr_filter = srcu_dereference(kvm->arch.msr_filter, &kvm->srcu);
	if (!msr_filter) {
		allowed = true;
		goto out;
	}

	allowed = msr_filter->default_allow;
	ranges = msr_filter->ranges;

	for (i = 0; i < msr_filter->count; i++) {
		u32 start = ranges[i].base;
		u32 end = start + ranges[i].nmsrs;
		u32 flags = ranges[i].flags;
		unsigned long *bitmap = ranges[i].bitmap;

		if ((index >= start) && (index < end) && (flags & type)) {
			allowed = test_bit(index - start, bitmap);
			break;
		}
	}

out:
	srcu_read_unlock(&kvm->srcu, idx);

	return allowed;
#endif
}
EXPORT_SYMBOL_GPL(kvm_msr_allowed);

int kvm_emulate_rdmsr(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_rdmsr);

int kvm_emulate_wrmsr(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_wrmsr);

int kvm_emulate_invd(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_invd);

int kvm_handle_invalid_op(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_handle_invalid_op);

int kvm_emulate_mwait(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_mwait);

int kvm_emulate_monitor(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_monitor);

static bool kvm_is_vm_type_supported(unsigned long type)
{
	return type < 32 && (kvm_caps.supported_vm_types & BIT(type));
}

int kvm_emulate_wbinvd(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_wbinvd);

int kvm_x86_vendor_init(struct kvm_x86_init_ops *ops)
{
#ifdef __PKVM_HYP__
	int r;

	kvm_caps.max_guest_tsc_khz = tsc_khz;
	/*
	 * FIXME: Is it possible to leverage KVM_X86_SW_PROTECTED_VM rather than
	 * adding the new type KVM_X86_PKVM_PROTECTED_VM?
	 */
	kvm_caps.supported_vm_types = BIT(KVM_X86_DEFAULT_VM) |
				      BIT(KVM_X86_PKVM_PROTECTED_VM);
	if (IS_ENABLED(CONFIG_KVM_SW_PROTECTED_VM))
		kvm_caps.supported_vm_types |= BIT(KVM_X86_SW_PROTECTED_VM);

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

int kvm_emulate_hypercall(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_hypercall);

int kvm_emulate_halt(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_halt);

int kvm_arch_vcpu_create(struct kvm_vcpu *vcpu)
{
#ifdef __PKVM_HYP__
	vcpu->arch.last_vmentry_cpu = -1;
	vcpu->arch.regs_avail = ~0;
	vcpu->arch.regs_dirty = ~0;

	return kvm_x86_call(vcpu_create)(vcpu);
#else
	struct page *page;
	int r;

	vcpu->arch.last_vmentry_cpu = -1;
	vcpu->arch.regs_avail = ~0;
	vcpu->arch.regs_dirty = ~0;

	kvm_gpc_init(&vcpu->arch.pv_time, vcpu->kvm);

	if (!irqchip_in_kernel(vcpu->kvm) || kvm_vcpu_is_reset_bsp(vcpu))
		vcpu->arch.mp_state = KVM_MP_STATE_RUNNABLE;
	else
		vcpu->arch.mp_state = KVM_MP_STATE_UNINITIALIZED;

	r = kvm_mmu_create(vcpu);
	if (r < 0)
		return r;

	r = kvm_create_lapic(vcpu);
	if (r < 0)
		goto fail_mmu_destroy;

	r = -ENOMEM;

	page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		goto fail_free_lapic;
	vcpu->arch.pio_data = page_address(page);

	vcpu->arch.mce_banks = kcalloc(KVM_MAX_MCE_BANKS * 4, sizeof(u64),
				       GFP_KERNEL_ACCOUNT);
	vcpu->arch.mci_ctl2_banks = kcalloc(KVM_MAX_MCE_BANKS, sizeof(u64),
					    GFP_KERNEL_ACCOUNT);
	if (!vcpu->arch.mce_banks || !vcpu->arch.mci_ctl2_banks)
		goto fail_free_mce_banks;
	vcpu->arch.mcg_cap = KVM_MAX_MCE_BANKS;

	if (!zalloc_cpumask_var(&vcpu->arch.wbinvd_dirty_mask,
				GFP_KERNEL_ACCOUNT))
		goto fail_free_mce_banks;

	if (!alloc_emulate_ctxt(vcpu))
		goto free_wbinvd_dirty_mask;

	if (!fpu_alloc_guest_fpstate(&vcpu->arch.guest_fpu)) {
		pr_err("failed to allocate vcpu's fpu\n");
		goto free_emulate_ctxt;
	}

	vcpu->arch.maxphyaddr = cpuid_query_maxphyaddr(vcpu);
	vcpu->arch.reserved_gpa_bits = kvm_vcpu_reserved_gpa_bits_raw(vcpu);

	kvm_async_pf_hash_reset(vcpu);

	vcpu->arch.perf_capabilities = kvm_caps.supported_perf_cap;
	kvm_pmu_init(vcpu);

	vcpu->arch.pending_external_vector = -1;
	vcpu->arch.preempted_in_kernel = false;

#if IS_ENABLED(CONFIG_HYPERV)
	vcpu->arch.hv_root_tdp = INVALID_PAGE;
#endif

	r = kvm_x86_call(vcpu_create)(vcpu);
	if (r)
		goto free_guest_fpu;

	vcpu->arch.arch_capabilities = kvm_get_arch_capabilities();
	vcpu->arch.msr_platform_info = MSR_PLATFORM_INFO_CPUID_FAULT;
	kvm_xen_init_vcpu(vcpu);
	vcpu_load(vcpu);
	kvm_set_tsc_khz(vcpu, vcpu->kvm->arch.default_tsc_khz);
	kvm_vcpu_reset(vcpu, false);
	kvm_init_mmu(vcpu);
	vcpu_put(vcpu);
	return 0;

free_guest_fpu:
	fpu_free_guest_fpstate(&vcpu->arch.guest_fpu);
free_emulate_ctxt:
	kmem_cache_free(x86_emulator_cache, vcpu->arch.emulate_ctxt);
free_wbinvd_dirty_mask:
	free_cpumask_var(vcpu->arch.wbinvd_dirty_mask);
fail_free_mce_banks:
	kfree(vcpu->arch.mce_banks);
	kfree(vcpu->arch.mci_ctl2_banks);
	free_page((unsigned long)vcpu->arch.pio_data);
fail_free_lapic:
	kvm_free_lapic(vcpu);
fail_mmu_destroy:
	kvm_mmu_destroy(vcpu);
	return r;
#endif
}

int kvm_arch_enable_virtualization_cpu(void)
{
#ifdef __PKVM_HYP__
	return kvm_x86_call(enable_virtualization_cpu)();
#else
	struct kvm *kvm;
	struct kvm_vcpu *vcpu;
	unsigned long i;
	int ret;
	u64 local_tsc;
	u64 max_tsc = 0;
	bool stable, backwards_tsc = false;

	kvm_user_return_msr_cpu_online();

	ret = kvm_x86_check_processor_compatibility();
	if (ret)
		return ret;

	ret = kvm_x86_call(enable_virtualization_cpu)();
	if (ret != 0)
		return ret;

	local_tsc = rdtsc();
	stable = !kvm_check_tsc_unstable();
	list_for_each_entry(kvm, &vm_list, vm_list) {
		kvm_for_each_vcpu(i, vcpu, kvm) {
			if (!stable && vcpu->cpu == smp_processor_id())
				kvm_make_request(KVM_REQ_CLOCK_UPDATE, vcpu);
			if (stable && vcpu->arch.last_host_tsc > local_tsc) {
				backwards_tsc = true;
				if (vcpu->arch.last_host_tsc > max_tsc)
					max_tsc = vcpu->arch.last_host_tsc;
			}
		}
	}

	/*
	 * Sometimes, even reliable TSCs go backwards.  This happens on
	 * platforms that reset TSC during suspend or hibernate actions, but
	 * maintain synchronization.  We must compensate.  Fortunately, we can
	 * detect that condition here, which happens early in CPU bringup,
	 * before any KVM threads can be running.  Unfortunately, we can't
	 * bring the TSCs fully up to date with real time, as we aren't yet far
	 * enough into CPU bringup that we know how much real time has actually
	 * elapsed; our helper function, ktime_get_boottime_ns() will be using boot
	 * variables that haven't been updated yet.
	 *
	 * So we simply find the maximum observed TSC above, then record the
	 * adjustment to TSC in each VCPU.  When the VCPU later gets loaded,
	 * the adjustment will be applied.  Note that we accumulate
	 * adjustments, in case multiple suspend cycles happen before some VCPU
	 * gets a chance to run again.  In the event that no KVM threads get a
	 * chance to run, we will miss the entire elapsed period, as we'll have
	 * reset last_host_tsc, so VCPUs will not have the TSC adjusted and may
	 * loose cycle time.  This isn't too big a deal, since the loss will be
	 * uniform across all VCPUs (not to mention the scenario is extremely
	 * unlikely). It is possible that a second hibernate recovery happens
	 * much faster than a first, causing the observed TSC here to be
	 * smaller; this would require additional padding adjustment, which is
	 * why we set last_host_tsc to the local tsc observed here.
	 *
	 * N.B. - this code below runs only on platforms with reliable TSC,
	 * as that is the only way backwards_tsc is set above.  Also note
	 * that this runs for ALL vcpus, which is not a bug; all VCPUs should
	 * have the same delta_cyc adjustment applied if backwards_tsc
	 * is detected.  Note further, this adjustment is only done once,
	 * as we reset last_host_tsc on all VCPUs to stop this from being
	 * called multiple times (one for each physical CPU bringup).
	 *
	 * Platforms with unreliable TSCs don't have to deal with this, they
	 * will be compensated by the logic in vcpu_load, which sets the TSC to
	 * catchup mode.  This will catchup all VCPUs to real time, but cannot
	 * guarantee that they stay in perfect synchronization.
	 */
	if (backwards_tsc) {
		u64 delta_cyc = max_tsc - local_tsc;
		list_for_each_entry(kvm, &vm_list, vm_list) {
			kvm->arch.backwards_tsc_observed = true;
			kvm_for_each_vcpu(i, vcpu, kvm) {
				vcpu->arch.tsc_offset_adjustment += delta_cyc;
				vcpu->arch.last_host_tsc = local_tsc;
				kvm_make_request(KVM_REQ_MASTERCLOCK_UPDATE, vcpu);
			}

			/*
			 * We have to disable TSC offset matching.. if you were
			 * booting a VM while issuing an S4 host suspend....
			 * you may have some problem.  Solving this issue is
			 * left as an exercise to the reader.
			 */
			kvm->arch.last_tsc_nsec = 0;
			kvm->arch.last_tsc_write = 0;
		}

	}
	return 0;
#endif
}

void kvm_arch_disable_virtualization_cpu(void)
{
	kvm_x86_call(disable_virtualization_cpu)();
#ifndef __PKVM_HYP__
	drop_user_return_notifiers();
#endif
}

int kvm_arch_init_vm(struct kvm *kvm, unsigned long type)
{
#ifdef __PKVM_HYP__
	if (!kvm_is_vm_type_supported(type))
		return -EINVAL;

	kvm->arch.vm_type = type;
	return kvm_x86_call(vm_init)(kvm);
#else
	int ret;
	unsigned long flags;

	if (!kvm_is_vm_type_supported(type))
		return -EINVAL;

	kvm->arch.vm_type = type;
	kvm->arch.has_private_mem =
		(type == KVM_X86_SW_PROTECTED_VM);
	/* Decided by the vendor code for other VM types.  */
	kvm->arch.pre_fault_allowed =
		type == KVM_X86_DEFAULT_VM || type == KVM_X86_SW_PROTECTED_VM;

	ret = kvm_page_track_init(kvm);
	if (ret)
		goto out;

	kvm_mmu_init_vm(kvm);

	ret = kvm_x86_call(vm_init)(kvm);
	if (ret)
		goto out_uninit_mmu;

	INIT_HLIST_HEAD(&kvm->arch.mask_notifier_list);
	atomic_set(&kvm->arch.noncoherent_dma_count, 0);

	/* Reserve bit 0 of irq_sources_bitmap for userspace irq source */
	set_bit(KVM_USERSPACE_IRQ_SOURCE_ID, &kvm->arch.irq_sources_bitmap);
	/* Reserve bit 1 of irq_sources_bitmap for irqfd-resampler */
	set_bit(KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID,
		&kvm->arch.irq_sources_bitmap);

	raw_spin_lock_init(&kvm->arch.tsc_write_lock);
	mutex_init(&kvm->arch.apic_map_lock);
	seqcount_raw_spinlock_init(&kvm->arch.pvclock_sc, &kvm->arch.tsc_write_lock);
	kvm->arch.kvmclock_offset = -get_kvmclock_base_ns();

	raw_spin_lock_irqsave(&kvm->arch.tsc_write_lock, flags);
	pvclock_update_vm_gtod_copy(kvm);
	raw_spin_unlock_irqrestore(&kvm->arch.tsc_write_lock, flags);

	kvm->arch.default_tsc_khz = max_tsc_khz ? : tsc_khz;
	kvm->arch.apic_bus_cycle_ns = APIC_BUS_CYCLE_NS_DEFAULT;
	kvm->arch.guest_can_read_msr_platform_info = true;
	kvm->arch.enable_pmu = enable_pmu;

#if IS_ENABLED(CONFIG_HYPERV)
	spin_lock_init(&kvm->arch.hv_root_tdp_lock);
	kvm->arch.hv_root_tdp = INVALID_PAGE;
#endif

	INIT_DELAYED_WORK(&kvm->arch.kvmclock_update_work, kvmclock_update_fn);
	INIT_DELAYED_WORK(&kvm->arch.kvmclock_sync_work, kvmclock_sync_fn);

	kvm_apicv_init(kvm);
	kvm_hv_init_vm(kvm);
	kvm_xen_init_vm(kvm);

	return 0;

out_uninit_mmu:
	kvm_mmu_uninit_vm(kvm);
	kvm_page_track_cleanup(kvm);
out:
	return ret;
#endif
}

void kvm_arch_destroy_vm(struct kvm *kvm)
{
#ifdef __PKVM_HYP__
	kvm_x86_call(vm_destroy)(kvm);
#else
	if (current->mm == kvm->mm) {
		/*
		 * Free memory regions allocated on behalf of userspace,
		 * unless the memory map has changed due to process exit
		 * or fd copying.
		 */
		mutex_lock(&kvm->slots_lock);
		__x86_set_memory_region(kvm, APIC_ACCESS_PAGE_PRIVATE_MEMSLOT,
					0, 0);
		__x86_set_memory_region(kvm, IDENTITY_PAGETABLE_PRIVATE_MEMSLOT,
					0, 0);
		__x86_set_memory_region(kvm, TSS_PRIVATE_MEMSLOT, 0, 0);
		mutex_unlock(&kvm->slots_lock);
	}
	kvm_unload_vcpu_mmus(kvm);
	kvm_x86_call(vm_destroy)(kvm);
	kvm_free_msr_filter(srcu_dereference_check(kvm->arch.msr_filter, &kvm->srcu, 1));
	kvm_pic_destroy(kvm);
	kvm_ioapic_destroy(kvm);
	kvm_destroy_vcpus(kvm);
	kvfree(rcu_dereference_check(kvm->arch.apic_map, 1));
	kfree(srcu_dereference_check(kvm->arch.pmu_event_filter, &kvm->srcu, 1));
	kvm_mmu_uninit_vm(kvm);
	kvm_page_track_cleanup(kvm);
	kvm_xen_destroy_vm(kvm);
	kvm_hv_destroy_vm(kvm);
	static_call_cond(kvm_x86_vm_free)(kvm);
#endif
}

#ifdef __PKVM_HYP__
void kvm_vcpu_enter_guest(struct kvm_vcpu *vcpu, bool force_immediate_exit)
{
	fastpath_t exit_fastpath;
	int ret;

	if (kvm_x86_call(vcpu_pre_run)(vcpu) <= 0)
		return;

	vcpu->arch.last_vmentry_cpu = vcpu->cpu;

	/* TODO: Save the host VMM fpu and load the guest fpu */

	vcpu->arch.host_debugctl = get_debugctlmsr();

	for (;;) {
		/*
		 * Make sure vcpu->mode is changed to IN_GUEST_MODE before
		 * running to mark this vcpu should be kicked for any new
		 * vcpu request.
		 */
		smp_store_mb(vcpu->mode, IN_GUEST_MODE);

		exit_fastpath = kvm_x86_call(vcpu_run)(vcpu, force_immediate_exit);

		/*
		 * Make sure vcpu->mode is changed to OUTSIDE_GUEST_MODE after
		 * vmexit to mark this vcpu no need to be kicked for any new
		 * vcpu request.
		 */
		smp_store_mb(vcpu->mode, OUTSIDE_GUEST_MODE);

		if (likely(exit_fastpath != EXIT_FASTPATH_REENTER_GUEST)) {
			ret = kvm_x86_call(handle_exit)(vcpu, exit_fastpath);
			if (ret <= 0)
				break;
		}
	}

	/* TODO: Restore the host VMM fpu and save the guest fpu */
}
#endif
