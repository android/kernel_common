// SPDX-License-Identifier: GPL-2.0
#include <linux/array_size.h>
#include <reverse_cpuid.h>
#include <asm/kvm_pkvm.h>
#include <asm/cpuid.h>
#include <pkvm/cpuid.h>
#include <mmu.h>
#include <pmu.h>
#include <hyperv.h>
#include <xen.h>

/*
 * Unlike "struct cpuinfo_x86.x86_capability", kvm_cpu_caps doesn't need to be
 * aligned to sizeof(unsigned long) because it's not accessed via bitops.
 */
u32 kvm_cpu_caps[NR_KVM_CPU_CAPS] __read_mostly;
EXPORT_SYMBOL_GPL(kvm_cpu_caps);

u32 xstate_required_size(u64 xstate_bv, bool compacted)
{
	int feature_bit = 0;
	u32 ret = XSAVE_HDR_SIZE + XSAVE_HDR_OFFSET;

	xstate_bv &= XFEATURE_MASK_EXTEND;
	while (xstate_bv) {
		if (xstate_bv & 0x1) {
			u32 eax, ebx, ecx, edx, offset;
			cpuid_count(0xD, feature_bit, &eax, &ebx, &ecx, &edx);
			/* ECX[1]: 64B alignment in compacted form */
			if (compacted)
				offset = (ecx & 0x2) ? ALIGN(ret, 64) : ret;
			else
				offset = ebx;
			ret = max(ret, offset + eax);
		}

		xstate_bv >>= 1;
		feature_bit++;
	}

	return ret;
}

#define F feature_bit

/* Scattered Flag - For features that are scattered by cpufeatures.h. */
#define SF(name)						\
({								\
	BUILD_BUG_ON(X86_FEATURE_##name >= MAX_CPU_FEATURES);	\
	(boot_cpu_has(X86_FEATURE_##name) ? F(name) : 0);	\
})

/*
 * Magic value used by KVM when querying userspace-provided CPUID entries and
 * doesn't care about the CPIUD index because the index of the function in
 * question is not significant.  Note, this magic value must have at least one
 * bit set in bits[63:32] and must be consumed as a u64 by cpuid_entry2_find()
 * to avoid false positives when processing guest CPUID input.
 */
#define KVM_CPUID_INDEX_NOT_SIGNIFICANT -1ull

static inline struct kvm_cpuid_entry2 *cpuid_entry2_find(
	struct kvm_cpuid_entry2 *entries, int nent, u32 function, u64 index)
{
	struct kvm_cpuid_entry2 *e;
	int i;

#ifndef __PKVM_HYP__
	/*
	 * KVM has a semi-arbitrary rule that querying the guest's CPUID model
	 * with IRQs disabled is disallowed.  The CPUID model can legitimately
	 * have over one hundred entries, i.e. the lookup is slow, and IRQs are
	 * typically disabled in KVM only when KVM is in a performance critical
	 * path, e.g. the core VM-Enter/VM-Exit run loop.  Nothing will break
	 * if this rule is violated, this assertion is purely to flag potential
	 * performance issues.  If this fires, consider moving the lookup out
	 * of the hotpath, e.g. by caching information during CPUID updates.
	 */
	lockdep_assert_irqs_enabled();
#endif

	for (i = 0; i < nent; i++) {
		e = &entries[i];

		if (e->function != function)
			continue;

		/*
		 * If the index isn't significant, use the first entry with a
		 * matching function.  It's userspace's responsibility to not
		 * provide "duplicate" entries in all cases.
		 */
		if (!(e->flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) || e->index == index)
			return e;


		/*
		 * Similarly, use the first matching entry if KVM is doing a
		 * lookup (as opposed to emulating CPUID) for a function that's
		 * architecturally defined as not having a significant index.
		 */
		if (index == KVM_CPUID_INDEX_NOT_SIGNIFICANT) {
			/*
			 * Direct lookups from KVM should not diverge from what
			 * KVM defines internally (the architectural behavior).
			 */
			WARN_ON_ONCE(cpuid_function_is_indexed(function));
			return e;
		}
	}

	return NULL;
}

static int kvm_check_cpuid(struct kvm_vcpu *vcpu,
			   struct kvm_cpuid_entry2 *entries,
			   int nent)
{
	struct kvm_cpuid_entry2 *best;
	u64 xfeatures;

	/*
	 * The existing code assumes virtual address is 48-bit or 57-bit in the
	 * canonical address checks; exit if it is ever changed.
	 */
	best = cpuid_entry2_find(entries, nent, 0x80000008,
				 KVM_CPUID_INDEX_NOT_SIGNIFICANT);
	if (best) {
		int vaddr_bits = (best->eax & 0xff00) >> 8;

		if (vaddr_bits != 48 && vaddr_bits != 57 && vaddr_bits != 0)
			return -EINVAL;
	}

	/*
	 * Exposing dynamic xfeatures to the guest requires additional
	 * enabling in the FPU, e.g. to expand the guest XSAVE state size.
	 */
	best = cpuid_entry2_find(entries, nent, 0xd, 0);
	if (!best)
		return 0;

	xfeatures = best->eax | ((u64)best->edx << 32);
	xfeatures &= XFEATURE_MASK_USER_DYNAMIC;
	if (!xfeatures)
		return 0;

#ifdef __PKVM_HYP__
	/*
	 * TODO: The guest fpu xfd feature is enabled by the host when the host
	 * KVM run its kvm_check_cpuid function before calling the
	 * vcpu_after_set_cpuid PV interface. Revisit when implements the fpu
	 * isolation.
	 */
	return 0;
#else
	return fpu_enable_guest_xfd_features(&vcpu->arch.guest_fpu, xfeatures);
#endif
}

/* Check whether the supplied CPUID data is equal to what is already set for the vCPU. */
static int kvm_cpuid_check_equal(struct kvm_vcpu *vcpu, struct kvm_cpuid_entry2 *e2,
				 int nent)
{
	struct kvm_cpuid_entry2 *orig;
	int i;

	if (nent != vcpu->arch.cpuid_nent)
		return -EINVAL;

	for (i = 0; i < nent; i++) {
		orig = &vcpu->arch.cpuid_entries[i];
		if (e2[i].function != orig->function ||
		    e2[i].index != orig->index ||
		    e2[i].flags != orig->flags ||
		    e2[i].eax != orig->eax || e2[i].ebx != orig->ebx ||
		    e2[i].ecx != orig->ecx || e2[i].edx != orig->edx)
			return -EINVAL;
	}

	return 0;
}

static struct kvm_hypervisor_cpuid __kvm_get_hypervisor_cpuid(struct kvm_cpuid_entry2 *entries,
							      int nent, const char *sig)
{
	struct kvm_hypervisor_cpuid cpuid = {};
	struct kvm_cpuid_entry2 *entry;
	u32 base;

	for_each_possible_hypervisor_cpuid_base(base) {
		entry = cpuid_entry2_find(entries, nent, base, KVM_CPUID_INDEX_NOT_SIGNIFICANT);

		if (entry) {
			u32 signature[3];

			signature[0] = entry->ebx;
			signature[1] = entry->ecx;
			signature[2] = entry->edx;

			if (!memcmp(signature, sig, sizeof(signature))) {
				cpuid.base = base;
				cpuid.limit = entry->eax;
				break;
			}
		}
	}

	return cpuid;
}

static struct kvm_hypervisor_cpuid kvm_get_hypervisor_cpuid(struct kvm_vcpu *vcpu,
							    const char *sig)
{
	return __kvm_get_hypervisor_cpuid(vcpu->arch.cpuid_entries,
					  vcpu->arch.cpuid_nent, sig);
}

static struct kvm_cpuid_entry2 *__kvm_find_kvm_cpuid_features(struct kvm_cpuid_entry2 *entries,
							      int nent, u32 kvm_cpuid_base)
{
	return cpuid_entry2_find(entries, nent, kvm_cpuid_base | KVM_CPUID_FEATURES,
				 KVM_CPUID_INDEX_NOT_SIGNIFICANT);
}

static struct kvm_cpuid_entry2 *kvm_find_kvm_cpuid_features(struct kvm_vcpu *vcpu)
{
	u32 base = vcpu->arch.kvm_cpuid.base;

	if (!base)
		return NULL;

	return __kvm_find_kvm_cpuid_features(vcpu->arch.cpuid_entries,
					     vcpu->arch.cpuid_nent, base);
}

void kvm_update_pv_runtime(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best = kvm_find_kvm_cpuid_features(vcpu);

	/*
	 * save the feature bitmap to avoid cpuid lookup for every PV
	 * operation
	 */
	if (best) {
#ifdef __PKVM_HYP__
		/*
		 * FIXME: Disable all PV features in the pkvm hypervisor to
		 * simplify the emulation. Can PV features be enabled for npVM?
		 * Can some of the PV features be enabled for pVM?
		 */
		best->eax = 0;
#endif
		vcpu->arch.pv_cpuid.features = best->eax;
	}
}

/*
 * Calculate guest's supported XCR0 taking into account guest CPUID data and
 * KVM's supported XCR0 (comprised of host's XCR0 and KVM_SUPPORTED_XCR0).
 */
static u64 cpuid_get_supported_xcr0(struct kvm_cpuid_entry2 *entries, int nent)
{
	struct kvm_cpuid_entry2 *best;

	best = cpuid_entry2_find(entries, nent, 0xd, 0);
	if (!best)
		return 0;

	return (best->eax | ((u64)best->edx << 32)) & kvm_caps.supported_xcr0;
}

static void __kvm_update_cpuid_runtime(struct kvm_vcpu *vcpu, struct kvm_cpuid_entry2 *entries,
				       int nent)
{
	struct kvm_cpuid_entry2 *best;
	struct kvm_hypervisor_cpuid kvm_cpuid;

	best = cpuid_entry2_find(entries, nent, 1, KVM_CPUID_INDEX_NOT_SIGNIFICANT);
	if (best) {
		/* Update OSXSAVE bit */
		if (boot_cpu_has(X86_FEATURE_XSAVE))
			cpuid_entry_change(best, X86_FEATURE_OSXSAVE,
					   kvm_is_cr4_bit_set(vcpu, X86_CR4_OSXSAVE));

		cpuid_entry_change(best, X86_FEATURE_APIC,
			   vcpu->arch.apic_base & MSR_IA32_APICBASE_ENABLE);
	}

	best = cpuid_entry2_find(entries, nent, 7, 0);
	if (best && boot_cpu_has(X86_FEATURE_PKU) && best->function == 0x7)
		cpuid_entry_change(best, X86_FEATURE_OSPKE,
				   kvm_is_cr4_bit_set(vcpu, X86_CR4_PKE));

	best = cpuid_entry2_find(entries, nent, 0xD, 0);
	if (best)
		best->ebx = xstate_required_size(vcpu->arch.xcr0, false);

	best = cpuid_entry2_find(entries, nent, 0xD, 1);
	if (best && (cpuid_entry_has(best, X86_FEATURE_XSAVES) ||
		     cpuid_entry_has(best, X86_FEATURE_XSAVEC)))
		best->ebx = xstate_required_size(vcpu->arch.xcr0, true);

	kvm_cpuid = __kvm_get_hypervisor_cpuid(entries, nent, KVM_SIGNATURE);
	if (kvm_cpuid.base) {
		best = __kvm_find_kvm_cpuid_features(entries, nent, kvm_cpuid.base);
		if (kvm_hlt_in_guest(vcpu->kvm) && best)
			best->eax &= ~(1 << KVM_FEATURE_PV_UNHALT);
	}

	if (!kvm_check_has_quirk(vcpu->kvm, KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT)) {
		best = cpuid_entry2_find(entries, nent, 0x1, KVM_CPUID_INDEX_NOT_SIGNIFICANT);
		if (best)
			cpuid_entry_change(best, X86_FEATURE_MWAIT,
					   vcpu->arch.ia32_misc_enable_msr &
					   MSR_IA32_MISC_ENABLE_MWAIT);
	}
}

void kvm_update_cpuid_runtime(struct kvm_vcpu *vcpu)
{
	__kvm_update_cpuid_runtime(vcpu, vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent);
}
EXPORT_SYMBOL_GPL(kvm_update_cpuid_runtime);

static bool kvm_cpuid_has_hyperv(struct kvm_cpuid_entry2 *entries, int nent)
{
#if defined(CONFIG_KVM_HYPERV) && !defined(__PKVM_HYP__)
	struct kvm_cpuid_entry2 *entry;

	entry = cpuid_entry2_find(entries, nent, HYPERV_CPUID_INTERFACE,
				  KVM_CPUID_INDEX_NOT_SIGNIFICANT);
	return entry && entry->eax == HYPERV_CPUID_SIGNATURE_EAX;
#else
	return false;
#endif
}

static bool guest_cpuid_is_amd_or_hygon(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *entry;

	entry = kvm_find_cpuid_entry(vcpu, 0);
	if (!entry)
		return false;

	return is_guest_vendor_amd(entry->ebx, entry->ecx, entry->edx) ||
	       is_guest_vendor_hygon(entry->ebx, entry->ecx, entry->edx);
}

static void kvm_vcpu_after_set_cpuid(struct kvm_vcpu *vcpu)
{
#ifndef __PKVM_HYP__
	struct kvm_lapic *apic = vcpu->arch.apic;
	struct kvm_cpuid_entry2 *best;
#endif
	bool allow_gbpages;

	BUILD_BUG_ON(KVM_NR_GOVERNED_FEATURES > KVM_MAX_NR_GOVERNED_FEATURES);
	bitmap_zero(vcpu->arch.governed_features.enabled,
		    KVM_MAX_NR_GOVERNED_FEATURES);

	/*
	 * If TDP is enabled, let the guest use GBPAGES if they're supported in
	 * hardware.  The hardware page walker doesn't let KVM disable GBPAGES,
	 * i.e. won't treat them as reserved, and KVM doesn't redo the GVA->GPA
	 * walk for performance and complexity reasons.  Not to mention KVM
	 * _can't_ solve the problem because GVA->GPA walks aren't visible to
	 * KVM once a TDP translation is installed.  Mimic hardware behavior so
	 * that KVM's is at least consistent, i.e. doesn't randomly inject #PF.
	 * If TDP is disabled, honor *only* guest CPUID as KVM has full control
	 * and can install smaller shadow pages if the host lacks 1GiB support.
	 */
	allow_gbpages = tdp_enabled ? boot_cpu_has(X86_FEATURE_GBPAGES) :
				      guest_cpuid_has(vcpu, X86_FEATURE_GBPAGES);
	if (allow_gbpages)
		kvm_governed_feature_set(vcpu, X86_FEATURE_GBPAGES);

#ifndef __PKVM_HYP__
	best = kvm_find_cpuid_entry(vcpu, 1);
	if (best && apic) {
		if (cpuid_entry_has(best, X86_FEATURE_TSC_DEADLINE_TIMER))
			apic->lapic_timer.timer_mode_mask = 3 << 17;
		else
			apic->lapic_timer.timer_mode_mask = 1 << 17;

		kvm_apic_set_version(vcpu);
	}
#endif
	vcpu->arch.guest_supported_xcr0 =
		cpuid_get_supported_xcr0(vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent);

	kvm_update_pv_runtime(vcpu);

	vcpu->arch.is_amd_compatible = guest_cpuid_is_amd_or_hygon(vcpu);
	vcpu->arch.maxphyaddr = cpuid_query_maxphyaddr(vcpu);
	vcpu->arch.reserved_gpa_bits = kvm_vcpu_reserved_gpa_bits_raw(vcpu);

#ifndef __PKVM_HYP__
	kvm_pmu_refresh(vcpu);
#endif
	vcpu->arch.cr4_guest_rsvd_bits =
	    __cr4_reserved_bits(guest_cpuid_has, vcpu);

	kvm_hv_set_cpuid(vcpu, kvm_cpuid_has_hyperv(vcpu->arch.cpuid_entries,
						    vcpu->arch.cpuid_nent));

#ifdef __PKVM_HYP__
	/* Disable the VMX as no nested support in the pkvm hypervisor */
	guest_cpuid_clear(vcpu, X86_FEATURE_VMX);
#endif
	/* Invoke the vendor callback only after the above state is updated. */
	kvm_x86_call(vcpu_after_set_cpuid)(vcpu);

	/* FIXME: How to do this with PV EPT? */
#ifndef __PKVM_HYP__
	/*
	 * Except for the MMU, which needs to do its thing any vendor specific
	 * adjustments to the reserved GPA bits.
	 */
	kvm_mmu_after_set_cpuid(vcpu);
#endif
}

int cpuid_query_maxphyaddr(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best;

	best = kvm_find_cpuid_entry(vcpu, 0x80000000);
	if (!best || best->eax < 0x80000008)
		goto not_found;
	best = kvm_find_cpuid_entry(vcpu, 0x80000008);
	if (best)
		return best->eax & 0xff;
not_found:
	return 36;
}

/*
 * This "raw" version returns the reserved GPA bits without any adjustments for
 * encryption technologies that usurp bits.  The raw mask should be used if and
 * only if hardware does _not_ strip the usurped bits, e.g. in virtual MTRRs.
 */
u64 kvm_vcpu_reserved_gpa_bits_raw(struct kvm_vcpu *vcpu)
{
	return rsvd_bits(cpuid_maxphyaddr(vcpu), 63);
}

int kvm_set_cpuid(struct kvm_vcpu *vcpu, struct kvm_cpuid_entry2 *e2, int nent)
{
	int r;

	__kvm_update_cpuid_runtime(vcpu, e2, nent);

	/*
	 * KVM does not correctly handle changing guest CPUID after KVM_RUN, as
	 * MAXPHYADDR, GBPAGES support, AMD reserved bit behavior, etc.. aren't
	 * tracked in kvm_mmu_page_role.  As a result, KVM may miss guest page
	 * faults due to reusing SPs/SPTEs. In practice no sane VMM mucks with
	 * the core vCPU model on the fly. It would've been better to forbid any
	 * KVM_SET_CPUID{,2} calls after KVM_RUN altogether but unfortunately
	 * some VMMs (e.g. QEMU) reuse vCPU fds for CPU hotplug/unplug and do
	 * KVM_SET_CPUID{,2} again. To support this legacy behavior, check
	 * whether the supplied CPUID data is equal to what's already set.
	 */
	if (kvm_vcpu_has_run(vcpu)) {
		r = kvm_cpuid_check_equal(vcpu, e2, nent);
		if (r)
			return r;

#ifndef __PKVM_HYP__
		kvfree(e2);
#endif
		return 0;
	}

#if defined(CONFIG_KVM_HYPERV)
	if (kvm_cpuid_has_hyperv(e2, nent)) {
		r = kvm_hv_vcpu_init(vcpu);
		if (r)
			return r;
	}
#endif

	r = kvm_check_cpuid(vcpu, e2, nent);
	if (r)
		return r;

#ifndef __PKVM_HYP__
	kvfree(vcpu->arch.cpuid_entries);
#endif
	vcpu->arch.cpuid_entries = e2;
	vcpu->arch.cpuid_nent = nent;

	vcpu->arch.kvm_cpuid = kvm_get_hypervisor_cpuid(vcpu, KVM_SIGNATURE);
#ifdef CONFIG_KVM_XEN
	vcpu->arch.xen.cpuid = kvm_get_hypervisor_cpuid(vcpu, XEN_SIGNATURE);
#endif
	kvm_vcpu_after_set_cpuid(vcpu);

	return 0;
}

/* Mask kvm_cpu_caps for @leaf with the raw CPUID capabilities of this CPU. */
static __always_inline void __kvm_cpu_cap_mask(unsigned int leaf)
{
	const struct cpuid_reg cpuid = x86_feature_cpuid(leaf * 32);
	struct kvm_cpuid_entry2 entry;

	reverse_cpuid_check(leaf);

	cpuid_count(cpuid.function, cpuid.index,
		    &entry.eax, &entry.ebx, &entry.ecx, &entry.edx);

	kvm_cpu_caps[leaf] &= *__cpuid_entry_get_reg(&entry, cpuid.reg);
}

static __always_inline
void kvm_cpu_cap_init_kvm_defined(enum kvm_only_cpuid_leafs leaf, u32 mask)
{
	/* Use kvm_cpu_cap_mask for leafs that aren't KVM-only. */
	BUILD_BUG_ON(leaf < NCAPINTS);

	kvm_cpu_caps[leaf] = mask;

	__kvm_cpu_cap_mask(leaf);
}

static __always_inline void kvm_cpu_cap_mask(enum cpuid_leafs leaf, u32 mask)
{
	/* Use kvm_cpu_cap_init_kvm_defined for KVM-only leafs. */
	BUILD_BUG_ON(leaf >= NCAPINTS);

	kvm_cpu_caps[leaf] &= mask;

	__kvm_cpu_cap_mask(leaf);
}

void kvm_set_cpu_caps(void)
{
#ifdef CONFIG_X86_64
	unsigned int f_gbpages = F(GBPAGES);
	unsigned int f_lm = F(LM);
	unsigned int f_xfd = F(XFD);
#else
	unsigned int f_gbpages = 0;
	unsigned int f_lm = 0;
	unsigned int f_xfd = 0;
#endif
	memset(kvm_cpu_caps, 0, sizeof(kvm_cpu_caps));

	BUILD_BUG_ON(sizeof(kvm_cpu_caps) - (NKVMCAPINTS * sizeof(*kvm_cpu_caps)) >
		     sizeof(boot_cpu_data.x86_capability));

	memcpy(&kvm_cpu_caps, &boot_cpu_data.x86_capability,
	       sizeof(kvm_cpu_caps) - (NKVMCAPINTS * sizeof(*kvm_cpu_caps)));

	kvm_cpu_cap_mask(CPUID_1_ECX,
		/*
		 * NOTE: MONITOR (and MWAIT) are emulated as NOP, but *not*
		 * advertised to guests via CPUID!
		 */
		F(XMM3) | F(PCLMULQDQ) | 0 /* DTES64, MONITOR */ |
		0 /* DS-CPL, VMX, SMX, EST */ |
		0 /* TM2 */ | F(SSSE3) | 0 /* CNXT-ID */ | 0 /* Reserved */ |
		F(FMA) | F(CX16) | 0 /* xTPR Update */ | F(PDCM) |
		F(PCID) | 0 /* Reserved, DCA */ | F(XMM4_1) |
		F(XMM4_2) | F(X2APIC) | F(MOVBE) | F(POPCNT) |
		0 /* Reserved*/ | F(AES) | F(XSAVE) | 0 /* OSXSAVE */ | F(AVX) |
		F(F16C) | F(RDRAND)
	);
	/* KVM emulates x2apic in software irrespective of host support. */
	kvm_cpu_cap_set(X86_FEATURE_X2APIC);

	kvm_cpu_cap_mask(CPUID_1_EDX,
		F(FPU) | F(VME) | F(DE) | F(PSE) |
		F(TSC) | F(MSR) | F(PAE) | F(MCE) |
		F(CX8) | F(APIC) | 0 /* Reserved */ | F(SEP) |
		F(MTRR) | F(PGE) | F(MCA) | F(CMOV) |
		F(PAT) | F(PSE36) | 0 /* PSN */ | F(CLFLUSH) |
		0 /* Reserved, DS, ACPI */ | F(MMX) |
		F(FXSR) | F(XMM) | F(XMM2) | F(SELFSNOOP) |
		0 /* HTT, TM, Reserved, PBE */
	);

	kvm_cpu_cap_mask(CPUID_7_0_EBX,
		F(FSGSBASE) | F(SGX) | F(BMI1) | F(HLE) | F(AVX2) |
		F(FDP_EXCPTN_ONLY) | F(SMEP) | F(BMI2) | F(ERMS) | F(INVPCID) |
		F(RTM) | F(ZERO_FCS_FDS) | 0 /*MPX*/ | F(AVX512F) |
		F(AVX512DQ) | F(RDSEED) | F(ADX) | F(SMAP) | F(AVX512IFMA) |
		F(CLFLUSHOPT) | F(CLWB) | 0 /*INTEL_PT*/ | F(AVX512PF) |
		F(AVX512ER) | F(AVX512CD) | F(SHA_NI) | F(AVX512BW) |
		F(AVX512VL));

	kvm_cpu_cap_mask(CPUID_7_ECX,
		F(AVX512VBMI) | F(LA57) | F(PKU) | 0 /*OSPKE*/ | F(RDPID) |
		F(AVX512_VPOPCNTDQ) | F(UMIP) | F(AVX512_VBMI2) | F(GFNI) |
		F(VAES) | F(VPCLMULQDQ) | F(AVX512_VNNI) | F(AVX512_BITALG) |
		F(CLDEMOTE) | F(MOVDIRI) | F(MOVDIR64B) | 0 /*WAITPKG*/ |
		F(SGX_LC) | F(BUS_LOCK_DETECT)
	);
	/* Set LA57 based on hardware capability. */
	if (cpuid_ecx(7) & F(LA57))
		kvm_cpu_cap_set(X86_FEATURE_LA57);

	/*
	 * PKU not yet implemented for shadow paging and requires OSPKE
	 * to be set on the host. Clear it if that is not the case
	 */
	if (!tdp_enabled || !boot_cpu_has(X86_FEATURE_OSPKE))
		kvm_cpu_cap_clear(X86_FEATURE_PKU);

	kvm_cpu_cap_mask(CPUID_7_EDX,
		F(AVX512_4VNNIW) | F(AVX512_4FMAPS) | F(SPEC_CTRL) |
		F(SPEC_CTRL_SSBD) | F(ARCH_CAPABILITIES) | F(INTEL_STIBP) |
		F(MD_CLEAR) | F(AVX512_VP2INTERSECT) | F(FSRM) |
		F(SERIALIZE) | F(TSXLDTRK) | F(AVX512_FP16) |
		F(AMX_TILE) | F(AMX_INT8) | F(AMX_BF16) | F(FLUSH_L1D)
	);

	/* TSC_ADJUST and ARCH_CAPABILITIES are emulated in software. */
	kvm_cpu_cap_set(X86_FEATURE_TSC_ADJUST);
	kvm_cpu_cap_set(X86_FEATURE_ARCH_CAPABILITIES);

	if (boot_cpu_has(X86_FEATURE_IBPB) && boot_cpu_has(X86_FEATURE_IBRS))
		kvm_cpu_cap_set(X86_FEATURE_SPEC_CTRL);
	if (boot_cpu_has(X86_FEATURE_STIBP))
		kvm_cpu_cap_set(X86_FEATURE_INTEL_STIBP);
	if (boot_cpu_has(X86_FEATURE_AMD_SSBD))
		kvm_cpu_cap_set(X86_FEATURE_SPEC_CTRL_SSBD);

	kvm_cpu_cap_mask(CPUID_7_1_EAX,
		F(AVX_VNNI) | F(AVX512_BF16) | F(CMPCCXADD) |
		F(FZRM) | F(FSRS) | F(FSRC) |
		F(AMX_FP16) | F(AVX_IFMA) | F(LAM)
	);

	kvm_cpu_cap_init_kvm_defined(CPUID_7_1_EDX,
		F(AVX_VNNI_INT8) | F(AVX_NE_CONVERT) | F(PREFETCHITI) |
		F(AMX_COMPLEX) | F(AVX10)
	);

	kvm_cpu_cap_init_kvm_defined(CPUID_7_2_EDX,
		F(INTEL_PSFD) | F(IPRED_CTRL) | F(RRSBA_CTRL) | F(DDPD_U) |
		F(BHI_CTRL) | F(MCDT_NO)
	);

	kvm_cpu_cap_mask(CPUID_D_1_EAX,
		F(XSAVEOPT) | F(XSAVEC) | F(XGETBV1) | F(XSAVES) | f_xfd
	);

	kvm_cpu_cap_init_kvm_defined(CPUID_12_EAX,
		SF(SGX1) | SF(SGX2) | SF(SGX_EDECCSSA)
	);

	kvm_cpu_cap_init_kvm_defined(CPUID_24_0_EBX,
		F(AVX10_128) | F(AVX10_256) | F(AVX10_512)
	);

	kvm_cpu_cap_mask(CPUID_8000_0001_ECX,
		F(LAHF_LM) | F(CMP_LEGACY) | 0 /*SVM*/ | 0 /* ExtApicSpace */ |
		F(CR8_LEGACY) | F(ABM) | F(SSE4A) | F(MISALIGNSSE) |
		F(3DNOWPREFETCH) | F(OSVW) | 0 /* IBS */ | F(XOP) |
		0 /* SKINIT, WDT, LWP */ | F(FMA4) | F(TBM) |
		F(TOPOEXT) | 0 /* PERFCTR_CORE */
	);

	kvm_cpu_cap_mask(CPUID_8000_0001_EDX,
		F(FPU) | F(VME) | F(DE) | F(PSE) |
		F(TSC) | F(MSR) | F(PAE) | F(MCE) |
		F(CX8) | F(APIC) | 0 /* Reserved */ | F(SYSCALL) |
		F(MTRR) | F(PGE) | F(MCA) | F(CMOV) |
		F(PAT) | F(PSE36) | 0 /* Reserved */ |
		F(NX) | 0 /* Reserved */ | F(MMXEXT) | F(MMX) |
		F(FXSR) | F(FXSR_OPT) | f_gbpages | F(RDTSCP) |
		0 /* Reserved */ | f_lm | F(3DNOWEXT) | F(3DNOW)
	);

	if (!tdp_enabled && IS_ENABLED(CONFIG_X86_64))
		kvm_cpu_cap_set(X86_FEATURE_GBPAGES);

	kvm_cpu_cap_init_kvm_defined(CPUID_8000_0007_EDX,
		SF(CONSTANT_TSC)
	);

	kvm_cpu_cap_mask(CPUID_8000_0008_EBX,
		F(CLZERO) | F(XSAVEERPTR) |
		F(WBNOINVD) | F(AMD_IBPB) | F(AMD_IBRS) | F(AMD_SSBD) | F(VIRT_SSBD) |
		F(AMD_SSB_NO) | F(AMD_STIBP) | F(AMD_STIBP_ALWAYS_ON) |
		F(AMD_PSFD)
	);

	/*
	 * AMD has separate bits for each SPEC_CTRL bit.
	 * arch/x86/kernel/cpu/bugs.c is kind enough to
	 * record that in cpufeatures so use them.
	 */
	if (boot_cpu_has(X86_FEATURE_IBPB))
		kvm_cpu_cap_set(X86_FEATURE_AMD_IBPB);
	if (boot_cpu_has(X86_FEATURE_IBRS))
		kvm_cpu_cap_set(X86_FEATURE_AMD_IBRS);
	if (boot_cpu_has(X86_FEATURE_STIBP))
		kvm_cpu_cap_set(X86_FEATURE_AMD_STIBP);
	if (boot_cpu_has(X86_FEATURE_SPEC_CTRL_SSBD))
		kvm_cpu_cap_set(X86_FEATURE_AMD_SSBD);
	if (!boot_cpu_has_bug(X86_BUG_SPEC_STORE_BYPASS))
		kvm_cpu_cap_set(X86_FEATURE_AMD_SSB_NO);
	/*
	 * The preference is to use SPEC CTRL MSR instead of the
	 * VIRT_SPEC MSR.
	 */
	if (boot_cpu_has(X86_FEATURE_LS_CFG_SSBD) &&
	    !boot_cpu_has(X86_FEATURE_AMD_SSBD))
		kvm_cpu_cap_set(X86_FEATURE_VIRT_SSBD);

	/*
	 * Hide all SVM features by default, SVM will set the cap bits for
	 * features it emulates and/or exposes for L1.
	 */
	kvm_cpu_cap_mask(CPUID_8000_000A_EDX, 0);

	kvm_cpu_cap_mask(CPUID_8000_001F_EAX,
		0 /* SME */ | 0 /* SEV */ | 0 /* VM_PAGE_FLUSH */ | 0 /* SEV_ES */ |
		F(SME_COHERENT));

	kvm_cpu_cap_mask(CPUID_8000_0021_EAX,
		F(NO_NESTED_DATA_BP) | F(LFENCE_RDTSC) | 0 /* SmmPgCfgLock */ |
		F(NULL_SEL_CLR_BASE) | F(AUTOIBRS) | 0 /* PrefetchCtlMsr */ |
		F(WRMSR_XX_BASE_NS)
	);

	kvm_cpu_cap_check_and_set(X86_FEATURE_SBPB);
	kvm_cpu_cap_check_and_set(X86_FEATURE_IBPB_BRTYPE);
	kvm_cpu_cap_check_and_set(X86_FEATURE_SRSO_NO);

	kvm_cpu_cap_init_kvm_defined(CPUID_8000_0022_EAX,
		F(PERFMON_V2)
	);

	/*
	 * Synthesize "LFENCE is serializing" into the AMD-defined entry in
	 * KVM's supported CPUID if the feature is reported as supported by the
	 * kernel.  LFENCE_RDTSC was a Linux-defined synthetic feature long
	 * before AMD joined the bandwagon, e.g. LFENCE is serializing on most
	 * CPUs that support SSE2.  On CPUs that don't support AMD's leaf,
	 * kvm_cpu_cap_mask() will unfortunately drop the flag due to ANDing
	 * the mask with the raw host CPUID, and reporting support in AMD's
	 * leaf can make it easier for userspace to detect the feature.
	 */
	if (cpu_feature_enabled(X86_FEATURE_LFENCE_RDTSC))
		kvm_cpu_cap_set(X86_FEATURE_LFENCE_RDTSC);
	if (!static_cpu_has_bug(X86_BUG_NULL_SEG))
		kvm_cpu_cap_set(X86_FEATURE_NULL_SEL_CLR_BASE);
	kvm_cpu_cap_set(X86_FEATURE_NO_SMM_CTL_MSR);

	kvm_cpu_cap_mask(CPUID_C000_0001_EDX,
		F(XSTORE) | F(XSTORE_EN) | F(XCRYPT) | F(XCRYPT_EN) |
		F(ACE2) | F(ACE2_EN) | F(PHE) | F(PHE_EN) |
		F(PMM) | F(PMM_EN)
	);

	/*
	 * Hide RDTSCP and RDPID if either feature is reported as supported but
	 * probing MSR_TSC_AUX failed.  This is purely a sanity check and
	 * should never happen, but the guest will likely crash if RDTSCP or
	 * RDPID is misreported, and KVM has botched MSR_TSC_AUX emulation in
	 * the past.  For example, the sanity check may fire if this instance of
	 * KVM is running as L1 on top of an older, broken KVM.
	 */
	if (WARN_ON((kvm_cpu_cap_has(X86_FEATURE_RDTSCP) ||
		     kvm_cpu_cap_has(X86_FEATURE_RDPID)) &&
		     !kvm_is_supported_user_return_msr(MSR_TSC_AUX))) {
		kvm_cpu_cap_clear(X86_FEATURE_RDTSCP);
		kvm_cpu_cap_clear(X86_FEATURE_RDPID);
	}
}
EXPORT_SYMBOL_GPL(kvm_set_cpu_caps);

struct kvm_cpuid_entry2 *kvm_find_cpuid_entry_index(struct kvm_vcpu *vcpu,
						    u32 function, u32 index)
{
	return cpuid_entry2_find(vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent,
				 function, index);
}
EXPORT_SYMBOL_GPL(kvm_find_cpuid_entry_index);

struct kvm_cpuid_entry2 *kvm_find_cpuid_entry(struct kvm_vcpu *vcpu,
					      u32 function)
{
	return cpuid_entry2_find(vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent,
				 function, KVM_CPUID_INDEX_NOT_SIGNIFICANT);
}
EXPORT_SYMBOL_GPL(kvm_find_cpuid_entry);

int kvm_emulate_cpuid(struct kvm_vcpu *vcpu)
{
#ifdef __PKVM_HYP__
	/*
	 * TODO:
	 * Overriding the KVM_CPUID_SIGNATURE leaf with
	 * "PKVMPKVMPKVM" is to make the guest aware that it is running
	 * with the protection from the pkvm hypervisor so it has to
	 * enable some enlightenment.
	 *
	 * To achieve this, is there any better solution rather than
	 * changing the KVM_CPUID_SIGNATURE leaf? Probably other leaf
	 * like 0x21 which is used by TDX guest?
	 */
	if ((kvm_rax_read(vcpu) == KVM_CPUID_SIGNATURE) && pkvm_is_protected_vcpu(vcpu)) {
		const u32 *sigptr = (const u32 *)"PKVMPKVMPKVM";

		vcpu->arch.regs[VCPU_REGS_RBX] = sigptr[0];
		vcpu->arch.regs[VCPU_REGS_RCX] = sigptr[1];
		vcpu->arch.regs[VCPU_REGS_RDX] = sigptr[2];

		return kvm_skip_emulated_instruction(vcpu);
	}
#endif
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_cpuid);
