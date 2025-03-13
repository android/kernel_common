// SPDX-License-Identifier: GPL-2.0
#include "x86.h"
#include <asm/debugreg.h>
#include <asm/fpu/xcr.h>
#include <asm/kvm_pkvm.h>
#include <cpuid.h>
#include <linux/user-return-notifier.h>
#include <trace.h>
#include <uapi/asm/debugreg.h>
#include <smm.h>
#include <lapic.h>
#include "pkvm.h"

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

static void __kvm_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags);

struct kvm_x86_ops kvm_x86_ops __read_mostly;

static bool __read_mostly ignore_msrs = 0;
module_param(ignore_msrs, bool, 0644);

bool __read_mostly report_ignored_msrs = true;
module_param(report_ignored_msrs, bool, 0644);
EXPORT_SYMBOL_GPL(report_ignored_msrs);

bool __read_mostly enable_vmware_backdoor;

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

struct kvm_user_return_msrs {
	struct user_return_notifier urn;
	bool registered;
	struct kvm_user_return_msr_values {
		u64 host;
		u64 curr;
	} values[KVM_MAX_NR_USER_RETURN_MSRS];
};

u32 __read_mostly kvm_nr_uret_msrs;
EXPORT_SYMBOL_GPL(kvm_nr_uret_msrs);
static u32 __read_mostly kvm_uret_msrs_list[KVM_MAX_NR_USER_RETURN_MSRS];
#ifdef __PKVM_HYP__
static DEFINE_PER_CPU(struct kvm_user_return_msrs, user_return_msrs);
#else
static struct kvm_user_return_msrs __percpu *user_return_msrs;
#endif

#define KVM_SUPPORTED_XCR0     (XFEATURE_MASK_FP | XFEATURE_MASK_SSE \
				| XFEATURE_MASK_YMM | XFEATURE_MASK_BNDREGS \
				| XFEATURE_MASK_BNDCSR | XFEATURE_MASK_AVX512 \
				| XFEATURE_MASK_PKRU | XFEATURE_MASK_XTILE)

bool __read_mostly allow_smaller_maxphyaddr = 0;
EXPORT_SYMBOL_GPL(allow_smaller_maxphyaddr);

bool __read_mostly enable_apicv = true;
EXPORT_SYMBOL_GPL(enable_apicv);

static bool kvm_is_advertised_msr(u32 msr_index)
{
#ifndef __PKVM_HYP__ /* FIXME: Is it necessary for the pkvm hypervisor to check these? */
	unsigned int i;

	for (i = 0; i < num_msrs_to_save; i++) {
		if (msrs_to_save[i] == msr_index)
			return true;
	}

	for (i = 0; i < num_emulated_msrs; i++) {
		if (emulated_msrs[i] == msr_index)
			return true;
	}
#endif

	return false;
}

typedef int (*msr_access_t)(struct kvm_vcpu *vcpu, u32 index, u64 *data,
			    bool host_initiated);

static __always_inline int kvm_do_msr_access(struct kvm_vcpu *vcpu, u32 msr,
					     u64 *data, bool host_initiated,
					     enum kvm_msr_access rw,
					     msr_access_t msr_access_fn)
{
	const char *op = rw == MSR_TYPE_W ? "wrmsr" : "rdmsr";
	int ret;

	BUILD_BUG_ON(rw != MSR_TYPE_R && rw != MSR_TYPE_W);

	/*
	 * Zero the data on read failures to avoid leaking stack data to the
	 * guest and/or userspace, e.g. if the failure is ignored below.
	 */
	ret = msr_access_fn(vcpu, msr, data, host_initiated);
	if (ret && rw == MSR_TYPE_R)
		*data = 0;

	if (ret != KVM_MSR_RET_UNSUPPORTED)
		return ret;

	/*
	 * Userspace is allowed to read MSRs, and write '0' to MSRs, that KVM
	 * advertises to userspace, even if an MSR isn't fully supported.
	 * Simply check that @data is '0', which covers both the write '0' case
	 * and all reads (in which case @data is zeroed on failure; see above).
	 */
	if (host_initiated && !*data && kvm_is_advertised_msr(msr))
		return 0;

	if (!ignore_msrs) {
		kvm_debug_ratelimited("unhandled %s: 0x%x data 0x%llx\n",
				      op, msr, *data);
		return ret;
	}

	if (report_ignored_msrs)
		kvm_pr_unimpl("ignored %s: 0x%x data 0x%llx\n", op, msr, *data);

	return 0;
}

static inline void kvm_async_pf_hash_reset(struct kvm_vcpu *vcpu)
{
	int i;
	for (i = 0; i < ASYNC_PF_PER_VCPU; i++)
		vcpu->arch.apf.gfns[i] = ~0;
}

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

static void kvm_user_return_msr_cpu_online(void)
{
#ifdef __PKVM_HYP__
	struct kvm_user_return_msrs *msrs = this_cpu_ptr(&user_return_msrs);
#else
	unsigned int cpu = smp_processor_id();
	struct kvm_user_return_msrs *msrs = per_cpu_ptr(user_return_msrs, cpu);
#endif
	u64 value;
	int i;

	for (i = 0; i < kvm_nr_uret_msrs; ++i) {
		rdmsrl_safe(kvm_uret_msrs_list[i], &value);
		msrs->values[i].host = value;
		msrs->values[i].curr = value;
	}
}

int kvm_set_user_return_msr(unsigned slot, u64 value, u64 mask)
{
#ifdef __PKVM_HYP__
	struct kvm_user_return_msrs *msrs = this_cpu_ptr(&user_return_msrs);
#else
	struct kvm_user_return_msrs *msrs = this_cpu_ptr(user_return_msrs);
#endif
	int err;

	value = (value & mask) | (msrs->values[slot].host & ~mask);
	if (value == msrs->values[slot].curr)
		return 0;
	err = wrmsrl_safe(kvm_uret_msrs_list[slot], value);
	if (err)
		return 1;

	msrs->values[slot].curr = value;
#ifndef __PKVM_HYP__
	if (!msrs->registered) {
		msrs->urn.on_user_return = kvm_on_user_return;
		user_return_notifier_register(&msrs->urn);
		msrs->registered = true;
	}
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_set_user_return_msr);

noinstr void kvm_spurious_fault(void)
{
#ifndef __PKVM_HYP__
	/* Fault while not rebooting.  We want the trace. */
	BUG_ON(!kvm_rebooting);
#endif
}
EXPORT_SYMBOL_GPL(kvm_spurious_fault);

#define EXCPT_BENIGN		0
#define EXCPT_CONTRIBUTORY	1
#define EXCPT_PF		2

static int exception_class(int vector)
{
	switch (vector) {
	case PF_VECTOR:
		return EXCPT_PF;
	case DE_VECTOR:
	case TS_VECTOR:
	case NP_VECTOR:
	case SS_VECTOR:
	case GP_VECTOR:
		return EXCPT_CONTRIBUTORY;
	default:
		break;
	}
	return EXCPT_BENIGN;
}

#define EXCPT_FAULT		0
#define EXCPT_TRAP		1
#define EXCPT_ABORT		2
#define EXCPT_INTERRUPT		3
#define EXCPT_DB		4

static int exception_type(int vector)
{
	unsigned int mask;

	if (WARN_ON(vector > 31 || vector == NMI_VECTOR))
		return EXCPT_INTERRUPT;

	mask = 1 << vector;

	/*
	 * #DBs can be trap-like or fault-like, the caller must check other CPU
	 * state, e.g. DR6, to determine whether a #DB is a trap or fault.
	 */
	if (mask & (1 << DB_VECTOR))
		return EXCPT_DB;

	if (mask & ((1 << BP_VECTOR) | (1 << OF_VECTOR)))
		return EXCPT_TRAP;

	if (mask & ((1 << DF_VECTOR) | (1 << MC_VECTOR)))
		return EXCPT_ABORT;

	/* Reserved exceptions will result in fault */
	return EXCPT_FAULT;
}

void kvm_deliver_exception_payload(struct kvm_vcpu *vcpu,
				   struct kvm_queued_exception *ex)
{
	if (!ex->has_payload)
		return;

	switch (ex->vector) {
	case DB_VECTOR:
		/*
		 * "Certain debug exceptions may clear bit 0-3.  The
		 * remaining contents of the DR6 register are never
		 * cleared by the processor".
		 */
		vcpu->arch.dr6 &= ~DR_TRAP_BITS;
		/*
		 * In order to reflect the #DB exception payload in guest
		 * dr6, three components need to be considered: active low
		 * bit, FIXED_1 bits and active high bits (e.g. DR6_BD,
		 * DR6_BS and DR6_BT)
		 * DR6_ACTIVE_LOW contains the FIXED_1 and active low bits.
		 * In the target guest dr6:
		 * FIXED_1 bits should always be set.
		 * Active low bits should be cleared if 1-setting in payload.
		 * Active high bits should be set if 1-setting in payload.
		 *
		 * Note, the payload is compatible with the pending debug
		 * exceptions/exit qualification under VMX, that active_low bits
		 * are active high in payload.
		 * So they need to be flipped for DR6.
		 */
		vcpu->arch.dr6 |= DR6_ACTIVE_LOW;
		vcpu->arch.dr6 |= ex->payload;
		vcpu->arch.dr6 ^= ex->payload & DR6_ACTIVE_LOW;

		/*
		 * The #DB payload is defined as compatible with the 'pending
		 * debug exceptions' field under VMX, not DR6. While bit 12 is
		 * defined in the 'pending debug exceptions' field (enabled
		 * breakpoint), it is reserved and must be zero in DR6.
		 */
		vcpu->arch.dr6 &= ~BIT(12);
		break;
	case PF_VECTOR:
		vcpu->arch.cr2 = ex->payload;
		break;
	}

	ex->has_payload = false;
	ex->payload = 0;
}
EXPORT_SYMBOL_GPL(kvm_deliver_exception_payload);

static void kvm_queue_exception_vmexit(struct kvm_vcpu *vcpu, unsigned int vector,
				       bool has_error_code, u32 error_code,
				       bool has_payload, unsigned long payload)
{
	struct kvm_queued_exception *ex = &vcpu->arch.exception_vmexit;

	ex->vector = vector;
	ex->injected = false;
	ex->pending = true;
	ex->has_error_code = has_error_code;
	ex->error_code = error_code;
	ex->has_payload = has_payload;
	ex->payload = payload;
}

static void kvm_multiple_exception(struct kvm_vcpu *vcpu,
		unsigned nr, bool has_error, u32 error_code,
		bool has_payload, unsigned long payload, bool reinject)
{
	u32 prev_nr;
	int class1, class2;

	kvm_make_request(KVM_REQ_EVENT, vcpu);

	/*
	 * If the exception is destined for L2 and isn't being reinjected,
	 * morph it to a VM-Exit if L1 wants to intercept the exception.  A
	 * previously injected exception is not checked because it was checked
	 * when it was original queued, and re-checking is incorrect if _L1_
	 * injected the exception, in which case it's exempt from interception.
	 */
	if (!reinject && is_guest_mode(vcpu) &&
	    kvm_x86_ops.nested_ops->is_exception_vmexit(vcpu, nr, error_code)) {
		kvm_queue_exception_vmexit(vcpu, nr, has_error, error_code,
					   has_payload, payload);
		return;
	}

	if (!vcpu->arch.exception.pending && !vcpu->arch.exception.injected) {
	queue:
		if (reinject) {
			/*
			 * On VM-Entry, an exception can be pending if and only
			 * if event injection was blocked by nested_run_pending.
			 * In that case, however, vcpu_enter_guest() requests an
			 * immediate exit, and the guest shouldn't proceed far
			 * enough to need reinjection.
			 */
			WARN_ON_ONCE(kvm_is_exception_pending(vcpu));
			vcpu->arch.exception.injected = true;
			if (WARN_ON_ONCE(has_payload)) {
				/*
				 * A reinjected event has already
				 * delivered its payload.
				 */
				has_payload = false;
				payload = 0;
			}
		} else {
			vcpu->arch.exception.pending = true;
			vcpu->arch.exception.injected = false;
		}
		vcpu->arch.exception.has_error_code = has_error;
		vcpu->arch.exception.vector = nr;
		vcpu->arch.exception.error_code = error_code;
		vcpu->arch.exception.has_payload = has_payload;
		vcpu->arch.exception.payload = payload;
		if (!is_guest_mode(vcpu))
			kvm_deliver_exception_payload(vcpu,
						      &vcpu->arch.exception);
		return;
	}

	/* to check exception */
	prev_nr = vcpu->arch.exception.vector;
	if (prev_nr == DF_VECTOR) {
		/* triple fault -> shutdown */
		kvm_make_request(KVM_REQ_TRIPLE_FAULT, vcpu);
		return;
	}
	class1 = exception_class(prev_nr);
	class2 = exception_class(nr);
	if ((class1 == EXCPT_CONTRIBUTORY && class2 == EXCPT_CONTRIBUTORY) ||
	    (class1 == EXCPT_PF && class2 != EXCPT_BENIGN)) {
		/*
		 * Synthesize #DF.  Clear the previously injected or pending
		 * exception so as not to incorrectly trigger shutdown.
		 */
		vcpu->arch.exception.injected = false;
		vcpu->arch.exception.pending = false;

		kvm_queue_exception_e(vcpu, DF_VECTOR, 0);
	} else {
		/* replace previous exception with a new one in a hope
		   that instruction re-execution will regenerate lost
		   exception */
		goto queue;
	}
}

void kvm_requeue_exception(struct kvm_vcpu *vcpu, unsigned nr)
{
	kvm_multiple_exception(vcpu, nr, false, 0, false, 0, true);
}
EXPORT_SYMBOL_GPL(kvm_requeue_exception);

int kvm_complete_insn_gp(struct kvm_vcpu *vcpu, int err)
{
	if (err)
		kvm_inject_gp(vcpu, 0);
	else
		return kvm_skip_emulated_instruction(vcpu);

	return 1;
}
EXPORT_SYMBOL_GPL(kvm_complete_insn_gp);

void kvm_queue_exception_e(struct kvm_vcpu *vcpu, unsigned nr, u32 error_code)
{
	kvm_multiple_exception(vcpu, nr, true, error_code, false, 0, false);
}
EXPORT_SYMBOL_GPL(kvm_queue_exception_e);

void kvm_requeue_exception_e(struct kvm_vcpu *vcpu, unsigned nr, u32 error_code)
{
	kvm_multiple_exception(vcpu, nr, true, error_code, false, 0, true);
}
EXPORT_SYMBOL_GPL(kvm_requeue_exception_e);

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

static int __kvm_set_xcr(struct kvm_vcpu *vcpu, u32 index, u64 xcr)
{
	u64 xcr0 = xcr;
	u64 old_xcr0 = vcpu->arch.xcr0;
	u64 valid_bits;

	/* Only support XCR_XFEATURE_ENABLED_MASK(xcr0) now  */
	if (index != XCR_XFEATURE_ENABLED_MASK)
		return 1;
	if (!(xcr0 & XFEATURE_MASK_FP))
		return 1;
	if ((xcr0 & XFEATURE_MASK_YMM) && !(xcr0 & XFEATURE_MASK_SSE))
		return 1;

	/*
	 * Do not allow the guest to set bits that we do not support
	 * saving.  However, xcr0 bit 0 is always set, even if the
	 * emulated CPU does not support XSAVE (see kvm_vcpu_reset()).
	 */
	valid_bits = vcpu->arch.guest_supported_xcr0 | XFEATURE_MASK_FP;
	if (xcr0 & ~valid_bits)
		return 1;

	if ((!(xcr0 & XFEATURE_MASK_BNDREGS)) !=
	    (!(xcr0 & XFEATURE_MASK_BNDCSR)))
		return 1;

	if (xcr0 & XFEATURE_MASK_AVX512) {
		if (!(xcr0 & XFEATURE_MASK_YMM))
			return 1;
		if ((xcr0 & XFEATURE_MASK_AVX512) != XFEATURE_MASK_AVX512)
			return 1;
	}

	if ((xcr0 & XFEATURE_MASK_XTILE) &&
	    ((xcr0 & XFEATURE_MASK_XTILE) != XFEATURE_MASK_XTILE))
		return 1;

	vcpu->arch.xcr0 = xcr0;

	if ((xcr0 ^ old_xcr0) & XFEATURE_MASK_EXTEND)
		kvm_update_cpuid_runtime(vcpu);
	return 0;
}

int kvm_emulate_xsetbv(struct kvm_vcpu *vcpu)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_emulate_xsetbv);

static void kvm_update_dr0123(struct kvm_vcpu *vcpu)
{
	int i;

	if (!(vcpu->guest_debug & KVM_GUESTDBG_USE_HW_BP)) {
		for (i = 0; i < KVM_NR_DB_REGS; i++)
			vcpu->arch.eff_db[i] = vcpu->arch.db[i];
	}
}

void kvm_update_dr7(struct kvm_vcpu *vcpu)
{
	unsigned long dr7;

	if (vcpu->guest_debug & KVM_GUESTDBG_USE_HW_BP)
		dr7 = vcpu->arch.guest_debug_dr7;
	else
		dr7 = vcpu->arch.dr7;
	kvm_x86_call(set_dr7)(vcpu, dr7);
	vcpu->arch.switch_db_regs &= ~KVM_DEBUGREG_BP_ENABLED;
	if (dr7 & DR7_BP_EN_MASK)
		vcpu->arch.switch_db_regs |= KVM_DEBUGREG_BP_ENABLED;
}
EXPORT_SYMBOL_GPL(kvm_update_dr7);

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

/*
 * Write @data into the MSR specified by @index.  Select MSR specific fault
 * checks are bypassed if @host_initiated is %true.
 * Returns 0 on success, non-0 otherwise.
 * Assumes vcpu_load() was already called.
 */
static int __kvm_set_msr(struct kvm_vcpu *vcpu, u32 index, u64 data,
			 bool host_initiated)
{
	struct msr_data msr;

	switch (index) {
	case MSR_FS_BASE:
	case MSR_GS_BASE:
	case MSR_KERNEL_GS_BASE:
	case MSR_CSTAR:
	case MSR_LSTAR:
		if (is_noncanonical_address(data, vcpu))
			return 1;
		break;
	case MSR_IA32_SYSENTER_EIP:
	case MSR_IA32_SYSENTER_ESP:
		/*
		 * IA32_SYSENTER_ESP and IA32_SYSENTER_EIP cause #GP if
		 * non-canonical address is written on Intel but not on
		 * AMD (which ignores the top 32-bits, because it does
		 * not implement 64-bit SYSENTER).
		 *
		 * 64-bit code should hence be able to write a non-canonical
		 * value on AMD.  Making the address canonical ensures that
		 * vmentry does not fail on Intel after writing a non-canonical
		 * value, and that something deterministic happens if the guest
		 * invokes 64-bit SYSENTER.
		 */
		data = __canonical_address(data, vcpu_virt_addr_bits(vcpu));
		break;
	case MSR_TSC_AUX:
		if (!kvm_is_supported_user_return_msr(MSR_TSC_AUX))
			return 1;

		if (!host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_RDTSCP) &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_RDPID))
			return 1;

		/*
		 * Per Intel's SDM, bits 63:32 are reserved, but AMD's APM has
		 * incomplete and conflicting architectural behavior.  Current
		 * AMD CPUs completely ignore bits 63:32, i.e. they aren't
		 * reserved and always read as zeros.  Enforce Intel's reserved
		 * bits check if the guest CPU is Intel compatible, otherwise
		 * clear the bits.  This ensures cross-vendor migration will
		 * provide consistent behavior for the guest.
		 */
		if (guest_cpuid_is_intel_compatible(vcpu) && (data >> 32) != 0)
			return 1;

		data = (u32)data;
		break;
	}

	msr.data = data;
	msr.index = index;
	msr.host_initiated = host_initiated;

	return kvm_x86_call(set_msr)(vcpu, &msr);
}

static int _kvm_set_msr(struct kvm_vcpu *vcpu, u32 index, u64 *data,
			bool host_initiated)
{
	return __kvm_set_msr(vcpu, index, *data, host_initiated);
}

static int kvm_set_msr_ignored_check(struct kvm_vcpu *vcpu,
				     u32 index, u64 data, bool host_initiated)
{
	return kvm_do_msr_access(vcpu, index, &data, host_initiated, MSR_TYPE_W,
				 _kvm_set_msr);
}

/*
 * Read the MSR specified by @index into @data.  Select MSR specific fault
 * checks are bypassed if @host_initiated is %true.
 * Returns 0 on success, non-0 otherwise.
 * Assumes vcpu_load() was already called.
 */
int __kvm_get_msr(struct kvm_vcpu *vcpu, u32 index, u64 *data,
		  bool host_initiated)
{
	struct msr_data msr;
	int ret;

	switch (index) {
	case MSR_TSC_AUX:
		if (!kvm_is_supported_user_return_msr(MSR_TSC_AUX))
			return 1;

		if (!host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_RDTSCP) &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_RDPID))
			return 1;
		break;
	}

	msr.index = index;
	msr.host_initiated = host_initiated;

	ret = kvm_x86_call(get_msr)(vcpu, &msr);
	if (!ret)
		*data = msr.data;
	return ret;
}

static int kvm_get_msr_ignored_check(struct kvm_vcpu *vcpu,
				     u32 index, u64 *data, bool host_initiated)
{
	return kvm_do_msr_access(vcpu, index, data, host_initiated, MSR_TYPE_R,
				 __kvm_get_msr);
}

int kvm_get_msr_with_filter(struct kvm_vcpu *vcpu, u32 index, u64 *data)
{
	if (!kvm_msr_allowed(vcpu, index, KVM_MSR_FILTER_READ))
		return KVM_MSR_RET_FILTERED;
	return kvm_get_msr_ignored_check(vcpu, index, data, false);
}
EXPORT_SYMBOL_GPL(kvm_get_msr_with_filter);

int kvm_set_msr_with_filter(struct kvm_vcpu *vcpu, u32 index, u64 data)
{
	if (!kvm_msr_allowed(vcpu, index, KVM_MSR_FILTER_WRITE))
		return KVM_MSR_RET_FILTERED;
	return kvm_set_msr_ignored_check(vcpu, index, data, false);
}
EXPORT_SYMBOL_GPL(kvm_set_msr_with_filter);

int kvm_emulate_rdmsr(struct kvm_vcpu *vcpu)
{
	u32 ecx = kvm_rcx_read(vcpu);
	u64 data;
	int r;

	r = kvm_get_msr_with_filter(vcpu, ecx, &data);

	if (!r) {
		trace_kvm_msr_read(ecx, data);

		kvm_rax_write(vcpu, data & -1u);
		kvm_rdx_write(vcpu, (data >> 32) & -1u);
	} else {
#ifdef __PKVM_HYP__
		/*
		 * The pkvm hypervisor handles the failed rdmsr emulation.
		 * If rdmsr emulation is unsupported, forward to the host VMM.
		 * TODO: Any other rdmsr emulation policy?
		 */
		if (r != 1)
			return 0;
#else
		/* MSR read failed? See if we should ask user space */
		if (kvm_msr_user_space(vcpu, ecx, KVM_EXIT_X86_RDMSR, 0,
				       complete_fast_rdmsr, r))
			return 0;
		trace_kvm_msr_read_ex(ecx);
#endif
	}

	return kvm_x86_call(complete_emulated_msr)(vcpu, r);
}
EXPORT_SYMBOL_GPL(kvm_emulate_rdmsr);

int kvm_emulate_wrmsr(struct kvm_vcpu *vcpu)
{
	u32 ecx = kvm_rcx_read(vcpu);
	u64 data = kvm_read_edx_eax(vcpu);
	int r;

	r = kvm_set_msr_with_filter(vcpu, ecx, data);

	if (!r) {
		trace_kvm_msr_write(ecx, data);
	} else {
#ifdef __PKVM_HYP__
		/*
		 * The pkvm hypervisor handles the failed wrmsr emulation.
		 * If wrmsr emulation is unsupported, forward to the host VMM.
		 * TODO: Any other wrmsr emulation policy?
		 */
		if (r != 1)
			return 0;
#else
		/* MSR write failed? See if we should ask user space */
		if (kvm_msr_user_space(vcpu, ecx, KVM_EXIT_X86_WRMSR, data,
				       complete_fast_msr_access, r))
			return 0;
		/* Signal all other negative errors to userspace */
		if (r < 0)
			return r;
		trace_kvm_msr_write_ex(ecx, data);
#endif
	}

	return kvm_x86_call(complete_emulated_msr)(vcpu, r);
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

static void kvmclock_reset(struct kvm_vcpu *vcpu)
{
	/* FIXME: No kvmclock support in the pkvm hypervisor. */
#ifndef __PKVM_HYP__
	kvm_gpc_deactivate(&vcpu->arch.pv_time);
	vcpu->arch.time = 0;
#endif
}

static void kvm_vcpu_flush_tlb_all(struct kvm_vcpu *vcpu)
{
	++vcpu->stat.tlb_flush;
	kvm_x86_call(flush_tlb_all)(vcpu);

	/* Flushing all ASIDs flushes the current ASID... */
	kvm_clear_request(KVM_REQ_TLB_FLUSH_CURRENT, vcpu);
}

static inline void kvm_vcpu_flush_tlb_current(struct kvm_vcpu *vcpu)
{
	++vcpu->stat.tlb_flush;
	kvm_x86_call(flush_tlb_current)(vcpu);
}

int kvm_set_msr_common(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	u32 msr = msr_info->index;
	u64 data = msr_info->data;

#ifndef __PKVM_HYP__
	if (msr && msr == vcpu->kvm->arch.xen_hvm_config.msr)
		return kvm_xen_write_hypercall_page(vcpu, data);
#endif

	switch (msr) {
	case MSR_AMD64_NB_CFG:
	case MSR_IA32_UCODE_WRITE:
	case MSR_VM_HSAVE_PA:
	case MSR_AMD64_PATCH_LOADER:
	case MSR_AMD64_BU_CFG2:
	case MSR_AMD64_DC_CFG:
	case MSR_AMD64_TW_CFG:
	case MSR_F15H_EX_CFG:
		break;

#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
	case MSR_IA32_UCODE_REV:
		if (msr_info->host_initiated)
			vcpu->arch.microcode_version = data;
		break;
	case MSR_IA32_ARCH_CAPABILITIES:
		if (!msr_info->host_initiated)
			return 1;
		vcpu->arch.arch_capabilities = data;
		break;
	case MSR_IA32_PERF_CAPABILITIES:
		if (!msr_info->host_initiated)
			return 1;
		if (data & ~kvm_caps.supported_perf_cap)
			return 1;

		/*
		 * Note, this is not just a performance optimization!  KVM
		 * disallows changing feature MSRs after the vCPU has run; PMU
		 * refresh will bug the VM if called after the vCPU has run.
		 */
		if (vcpu->arch.perf_capabilities == data)
			break;

		vcpu->arch.perf_capabilities = data;
		kvm_pmu_refresh(vcpu);
		break;
	case MSR_IA32_PRED_CMD: {
		u64 reserved_bits = ~(PRED_CMD_IBPB | PRED_CMD_SBPB);

		if (!msr_info->host_initiated) {
			if ((!guest_has_pred_cmd_msr(vcpu)))
				return 1;

			if (!guest_cpuid_has(vcpu, X86_FEATURE_SPEC_CTRL) &&
			    !guest_cpuid_has(vcpu, X86_FEATURE_AMD_IBPB))
				reserved_bits |= PRED_CMD_IBPB;

			if (!guest_cpuid_has(vcpu, X86_FEATURE_SBPB))
				reserved_bits |= PRED_CMD_SBPB;
		}

		if (!boot_cpu_has(X86_FEATURE_IBPB))
			reserved_bits |= PRED_CMD_IBPB;

		if (!boot_cpu_has(X86_FEATURE_SBPB))
			reserved_bits |= PRED_CMD_SBPB;

		if (data & reserved_bits)
			return 1;

		if (!data)
			break;

		wrmsrl(MSR_IA32_PRED_CMD, data);
		break;
	}
	case MSR_IA32_FLUSH_CMD:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_FLUSH_L1D))
			return 1;

		if (!boot_cpu_has(X86_FEATURE_FLUSH_L1D) || (data & ~L1D_FLUSH))
			return 1;
		if (!data)
			break;

		wrmsrl(MSR_IA32_FLUSH_CMD, L1D_FLUSH);
		break;
	case MSR_EFER:
		return set_efer(vcpu, msr_info);
	case MSR_K7_HWCR:
		data &= ~(u64)0x40;	/* ignore flush filter disable */
		data &= ~(u64)0x100;	/* ignore ignne emulation enable */
		data &= ~(u64)0x8;	/* ignore TLB cache disable */

		/*
		 * Allow McStatusWrEn and TscFreqSel. (Linux guests from v3.2
		 * through at least v6.6 whine if TscFreqSel is clear,
		 * depending on F/M/S.
		 */
		if (data & ~(BIT_ULL(18) | BIT_ULL(24))) {
			kvm_pr_unimpl_wrmsr(vcpu, msr, data);
			return 1;
		}
		vcpu->arch.msr_hwcr = data;
		break;
	case MSR_FAM10H_MMIO_CONF_BASE:
		if (data != 0) {
			kvm_pr_unimpl_wrmsr(vcpu, msr, data);
			return 1;
		}
		break;
#endif
	case MSR_IA32_CR_PAT:
		if (!kvm_pat_valid(data))
			return 1;

		vcpu->arch.pat = data;
		break;
#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
	case MTRRphysBase_MSR(0) ... MSR_MTRRfix4K_F8000:
	case MSR_MTRRdefType:
		return kvm_mtrr_set_msr(vcpu, msr, data);
	case MSR_IA32_APICBASE:
		return kvm_set_apic_base(vcpu, msr_info);
	case APIC_BASE_MSR ... APIC_BASE_MSR + 0xff:
		return kvm_x2apic_msr_write(vcpu, msr, data);
	case MSR_IA32_TSC_DEADLINE:
		kvm_set_lapic_tscdeadline_msr(vcpu, data);
		break;
	case MSR_IA32_TSC_ADJUST:
		if (guest_cpuid_has(vcpu, X86_FEATURE_TSC_ADJUST)) {
			if (!msr_info->host_initiated) {
				s64 adj = data - vcpu->arch.ia32_tsc_adjust_msr;
				adjust_tsc_offset_guest(vcpu, adj);
				/* Before back to guest, tsc_timestamp must be adjusted
				 * as well, otherwise guest's percpu pvclock time could jump.
				 */
				kvm_make_request(KVM_REQ_CLOCK_UPDATE, vcpu);
			}
			vcpu->arch.ia32_tsc_adjust_msr = data;
		}
		break;
	case MSR_IA32_MISC_ENABLE: {
		u64 old_val = vcpu->arch.ia32_misc_enable_msr;

		if (!msr_info->host_initiated) {
			/* RO bits */
			if ((old_val ^ data) & MSR_IA32_MISC_ENABLE_PMU_RO_MASK)
				return 1;

			/* R bits, i.e. writes are ignored, but don't fault. */
			data = data & ~MSR_IA32_MISC_ENABLE_EMON;
			data |= old_val & MSR_IA32_MISC_ENABLE_EMON;
		}

		if (!kvm_check_has_quirk(vcpu->kvm, KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT) &&
		    ((old_val ^ data)  & MSR_IA32_MISC_ENABLE_MWAIT)) {
			if (!guest_cpuid_has(vcpu, X86_FEATURE_XMM3))
				return 1;
			vcpu->arch.ia32_misc_enable_msr = data;
			kvm_update_cpuid_runtime(vcpu);
		} else {
			vcpu->arch.ia32_misc_enable_msr = data;
		}
		break;
	}
	case MSR_IA32_SMBASE:
		if (!IS_ENABLED(CONFIG_KVM_SMM) || !msr_info->host_initiated)
			return 1;
		vcpu->arch.smbase = data;
		break;
	case MSR_IA32_POWER_CTL:
		vcpu->arch.msr_ia32_power_ctl = data;
		break;
	case MSR_IA32_TSC:
		if (msr_info->host_initiated) {
			kvm_synchronize_tsc(vcpu, &data);
		} else {
			u64 adj = kvm_compute_l1_tsc_offset(vcpu, data) - vcpu->arch.l1_tsc_offset;
			adjust_tsc_offset_guest(vcpu, adj);
			vcpu->arch.ia32_tsc_adjust_msr += adj;
		}
		break;
#endif
	case MSR_IA32_XSS:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_XSAVES))
			return 1;
		/*
		 * KVM supports exposing PT to the guest, but does not support
		 * IA32_XSS[bit 8]. Guests have to use RDMSR/WRMSR rather than
		 * XSAVES/XRSTORS to save/restore PT MSRs.
		 */
		if (data & ~kvm_caps.supported_xss)
			return 1;
		vcpu->arch.ia32_xss = data;
		kvm_update_cpuid_runtime(vcpu);
		break;
#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
	case MSR_SMI_COUNT:
		if (!msr_info->host_initiated)
			return 1;
		vcpu->arch.smi_count = data;
		break;
	case MSR_KVM_WALL_CLOCK_NEW:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE2))
			return 1;

		vcpu->kvm->arch.wall_clock = data;
		kvm_write_wall_clock(vcpu->kvm, data, 0);
		break;
	case MSR_KVM_WALL_CLOCK:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE))
			return 1;

		vcpu->kvm->arch.wall_clock = data;
		kvm_write_wall_clock(vcpu->kvm, data, 0);
		break;
	case MSR_KVM_SYSTEM_TIME_NEW:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE2))
			return 1;

		kvm_write_system_time(vcpu, data, false, msr_info->host_initiated);
		break;
	case MSR_KVM_SYSTEM_TIME:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE))
			return 1;

		kvm_write_system_time(vcpu, data, true,  msr_info->host_initiated);
		break;
	case MSR_KVM_ASYNC_PF_EN:
		if (!guest_pv_has(vcpu, KVM_FEATURE_ASYNC_PF))
			return 1;

		if (kvm_pv_enable_async_pf(vcpu, data))
			return 1;
		break;
	case MSR_KVM_ASYNC_PF_INT:
		if (!guest_pv_has(vcpu, KVM_FEATURE_ASYNC_PF_INT))
			return 1;

		if (kvm_pv_enable_async_pf_int(vcpu, data))
			return 1;
		break;
	case MSR_KVM_ASYNC_PF_ACK:
		if (!guest_pv_has(vcpu, KVM_FEATURE_ASYNC_PF_INT))
			return 1;
		if (data & 0x1) {
			vcpu->arch.apf.pageready_pending = false;
			kvm_check_async_pf_completion(vcpu);
		}
		break;
	case MSR_KVM_STEAL_TIME:
		if (!guest_pv_has(vcpu, KVM_FEATURE_STEAL_TIME))
			return 1;

		if (unlikely(!sched_info_on()))
			return 1;

		if (data & KVM_STEAL_RESERVED_MASK)
			return 1;

		vcpu->arch.st.msr_val = data;

		if (!(data & KVM_MSR_ENABLED))
			break;

		kvm_make_request(KVM_REQ_STEAL_UPDATE, vcpu);

		break;
	case MSR_KVM_PV_EOI_EN:
		if (!guest_pv_has(vcpu, KVM_FEATURE_PV_EOI))
			return 1;

		if (kvm_lapic_set_pv_eoi(vcpu, data, sizeof(u8)))
			return 1;
		break;

	case MSR_KVM_POLL_CONTROL:
		if (!guest_pv_has(vcpu, KVM_FEATURE_POLL_CONTROL))
			return 1;

		/* only enable bit supported */
		if (data & (-1ULL << 1))
			return 1;

		vcpu->arch.msr_kvm_poll_control = data;
		break;

	case MSR_IA32_MCG_CTL:
	case MSR_IA32_MCG_STATUS:
	case MSR_IA32_MC0_CTL ... MSR_IA32_MCx_CTL(KVM_MAX_MCE_BANKS) - 1:
	case MSR_IA32_MC0_CTL2 ... MSR_IA32_MCx_CTL2(KVM_MAX_MCE_BANKS) - 1:
		return set_msr_mce(vcpu, msr_info);

	case MSR_K7_PERFCTR0 ... MSR_K7_PERFCTR3:
	case MSR_P6_PERFCTR0 ... MSR_P6_PERFCTR1:
	case MSR_K7_EVNTSEL0 ... MSR_K7_EVNTSEL3:
	case MSR_P6_EVNTSEL0 ... MSR_P6_EVNTSEL1:
		if (kvm_pmu_is_valid_msr(vcpu, msr))
			return kvm_pmu_set_msr(vcpu, msr_info);

		if (data)
			kvm_pr_unimpl_wrmsr(vcpu, msr, data);
		break;
	case MSR_K7_CLK_CTL:
		/*
		 * Ignore all writes to this no longer documented MSR.
		 * Writes are only relevant for old K7 processors,
		 * all pre-dating SVM, but a recommended workaround from
		 * AMD for these chips. It is possible to specify the
		 * affected processor models on the command line, hence
		 * the need to ignore the workaround.
		 */
		break;
#ifdef CONFIG_KVM_HYPERV
	case HV_X64_MSR_GUEST_OS_ID ... HV_X64_MSR_SINT15:
	case HV_X64_MSR_SYNDBG_CONTROL ... HV_X64_MSR_SYNDBG_PENDING_BUFFER:
	case HV_X64_MSR_SYNDBG_OPTIONS:
	case HV_X64_MSR_CRASH_P0 ... HV_X64_MSR_CRASH_P4:
	case HV_X64_MSR_CRASH_CTL:
	case HV_X64_MSR_STIMER0_CONFIG ... HV_X64_MSR_STIMER3_COUNT:
	case HV_X64_MSR_REENLIGHTENMENT_CONTROL:
	case HV_X64_MSR_TSC_EMULATION_CONTROL:
	case HV_X64_MSR_TSC_EMULATION_STATUS:
	case HV_X64_MSR_TSC_INVARIANT_CONTROL:
		return kvm_hv_set_msr_common(vcpu, msr, data,
					     msr_info->host_initiated);
#endif
	case MSR_IA32_BBL_CR_CTL3:
		/* Drop writes to this legacy MSR -- see rdmsr
		 * counterpart for further detail.
		 */
		kvm_pr_unimpl_wrmsr(vcpu, msr, data);
		break;
	case MSR_AMD64_OSVW_ID_LENGTH:
		if (!guest_cpuid_has(vcpu, X86_FEATURE_OSVW))
			return 1;
		vcpu->arch.osvw.length = data;
		break;
	case MSR_AMD64_OSVW_STATUS:
		if (!guest_cpuid_has(vcpu, X86_FEATURE_OSVW))
			return 1;
		vcpu->arch.osvw.status = data;
		break;
	case MSR_PLATFORM_INFO:
		if (!msr_info->host_initiated ||
		    (!(data & MSR_PLATFORM_INFO_CPUID_FAULT) &&
		     cpuid_fault_enabled(vcpu)))
			return 1;
		vcpu->arch.msr_platform_info = data;
		break;
	case MSR_MISC_FEATURES_ENABLES:
		if (data & ~MSR_MISC_FEATURES_ENABLES_CPUID_FAULT ||
		    (data & MSR_MISC_FEATURES_ENABLES_CPUID_FAULT &&
		     !supports_cpuid_fault(vcpu)))
			return 1;
		vcpu->arch.msr_misc_features_enables = data;
		break;
#ifdef CONFIG_X86_64
	case MSR_IA32_XFD:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_XFD))
			return 1;

		if (data & ~kvm_guest_supported_xfd(vcpu))
			return 1;

		fpu_update_guest_xfd(&vcpu->arch.guest_fpu, data);
		break;
	case MSR_IA32_XFD_ERR:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_XFD))
			return 1;

		if (data & ~kvm_guest_supported_xfd(vcpu))
			return 1;

		vcpu->arch.guest_fpu.xfd_err = data;
		break;
#endif
#endif
	default:
#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
		if (kvm_pmu_is_valid_msr(vcpu, msr))
			return kvm_pmu_set_msr(vcpu, msr_info);
#endif

		return KVM_MSR_RET_UNSUPPORTED;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_set_msr_common);

int kvm_get_msr_common(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	switch (msr_info->index) {
	case MSR_IA32_PLATFORM_ID:
	case MSR_IA32_EBL_CR_POWERON:
	case MSR_IA32_LASTBRANCHFROMIP:
	case MSR_IA32_LASTBRANCHTOIP:
	case MSR_IA32_LASTINTFROMIP:
	case MSR_IA32_LASTINTTOIP:
	case MSR_AMD64_SYSCFG:
	case MSR_K8_TSEG_ADDR:
	case MSR_K8_TSEG_MASK:
	case MSR_VM_HSAVE_PA:
	case MSR_K8_INT_PENDING_MSG:
	case MSR_AMD64_NB_CFG:
	case MSR_FAM10H_MMIO_CONF_BASE:
	case MSR_AMD64_BU_CFG2:
	case MSR_IA32_PERF_CTL:
	case MSR_AMD64_DC_CFG:
	case MSR_AMD64_TW_CFG:
	case MSR_F15H_EX_CFG:
	/*
	 * Intel Sandy Bridge CPUs must support the RAPL (running average power
	 * limit) MSRs. Just return 0, as we do not want to expose the host
	 * data here. Do not conditionalize this on CPUID, as KVM does not do
	 * so for existing CPU-specific MSRs.
	 */
	case MSR_RAPL_POWER_UNIT:
	case MSR_PP0_ENERGY_STATUS:	/* Power plane 0 (core) */
	case MSR_PP1_ENERGY_STATUS:	/* Power plane 1 (graphics uncore) */
	case MSR_PKG_ENERGY_STATUS:	/* Total package */
	case MSR_DRAM_ENERGY_STATUS:	/* DRAM controller */
		msr_info->data = 0;
		break;
#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
	case MSR_K7_EVNTSEL0 ... MSR_K7_EVNTSEL3:
	case MSR_K7_PERFCTR0 ... MSR_K7_PERFCTR3:
	case MSR_P6_PERFCTR0 ... MSR_P6_PERFCTR1:
	case MSR_P6_EVNTSEL0 ... MSR_P6_EVNTSEL1:
		if (kvm_pmu_is_valid_msr(vcpu, msr_info->index))
			return kvm_pmu_get_msr(vcpu, msr_info);
		msr_info->data = 0;
		break;
	case MSR_IA32_UCODE_REV:
		msr_info->data = vcpu->arch.microcode_version;
		break;
	case MSR_IA32_ARCH_CAPABILITIES:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_ARCH_CAPABILITIES))
			return 1;
		msr_info->data = vcpu->arch.arch_capabilities;
		break;
	case MSR_IA32_PERF_CAPABILITIES:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_PDCM))
			return 1;
		msr_info->data = vcpu->arch.perf_capabilities;
		break;
	case MSR_IA32_POWER_CTL:
		msr_info->data = vcpu->arch.msr_ia32_power_ctl;
		break;
	case MSR_IA32_TSC: {
		/*
		 * Intel SDM states that MSR_IA32_TSC read adds the TSC offset
		 * even when not intercepted. AMD manual doesn't explicitly
		 * state this but appears to behave the same.
		 *
		 * On userspace reads and writes, however, we unconditionally
		 * return L1's TSC value to ensure backwards-compatible
		 * behavior for migration.
		 */
		u64 offset, ratio;

		if (msr_info->host_initiated) {
			offset = vcpu->arch.l1_tsc_offset;
			ratio = vcpu->arch.l1_tsc_scaling_ratio;
		} else {
			offset = vcpu->arch.tsc_offset;
			ratio = vcpu->arch.tsc_scaling_ratio;
		}

		msr_info->data = kvm_scale_tsc(rdtsc(), ratio) + offset;
		break;
	}
#endif
	case MSR_IA32_CR_PAT:
		msr_info->data = vcpu->arch.pat;
		break;
#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
	case MSR_MTRRcap:
	case MTRRphysBase_MSR(0) ... MSR_MTRRfix4K_F8000:
	case MSR_MTRRdefType:
		return kvm_mtrr_get_msr(vcpu, msr_info->index, &msr_info->data);
	case 0xcd: /* fsb frequency */
		msr_info->data = 3;
		break;
		/*
		 * MSR_EBC_FREQUENCY_ID
		 * Conservative value valid for even the basic CPU models.
		 * Models 0,1: 000 in bits 23:21 indicating a bus speed of
		 * 100MHz, model 2 000 in bits 18:16 indicating 100MHz,
		 * and 266MHz for model 3, or 4. Set Core Clock
		 * Frequency to System Bus Frequency Ratio to 1 (bits
		 * 31:24) even though these are only valid for CPU
		 * models > 2, however guests may end up dividing or
		 * multiplying by zero otherwise.
		 */
	case MSR_EBC_FREQUENCY_ID:
		msr_info->data = 1 << 24;
		break;
	case MSR_IA32_APICBASE:
		msr_info->data = vcpu->arch.apic_base;
		break;
	case APIC_BASE_MSR ... APIC_BASE_MSR + 0xff:
		return kvm_x2apic_msr_read(vcpu, msr_info->index, &msr_info->data);
	case MSR_IA32_TSC_DEADLINE:
		msr_info->data = kvm_get_lapic_tscdeadline_msr(vcpu);
		break;
	case MSR_IA32_TSC_ADJUST:
		msr_info->data = (u64)vcpu->arch.ia32_tsc_adjust_msr;
		break;
	case MSR_IA32_MISC_ENABLE:
		msr_info->data = vcpu->arch.ia32_misc_enable_msr;
		break;
	case MSR_IA32_SMBASE:
		if (!IS_ENABLED(CONFIG_KVM_SMM) || !msr_info->host_initiated)
			return 1;
		msr_info->data = vcpu->arch.smbase;
		break;
	case MSR_SMI_COUNT:
		msr_info->data = vcpu->arch.smi_count;
		break;
	case MSR_IA32_PERF_STATUS:
		/* TSC increment by tick */
		msr_info->data = 1000ULL;
		/* CPU multiplier */
		msr_info->data |= (((uint64_t)4ULL) << 40);
		break;
	case MSR_EFER:
		msr_info->data = vcpu->arch.efer;
		break;
	case MSR_KVM_WALL_CLOCK:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE))
			return 1;

		msr_info->data = vcpu->kvm->arch.wall_clock;
		break;
	case MSR_KVM_WALL_CLOCK_NEW:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE2))
			return 1;

		msr_info->data = vcpu->kvm->arch.wall_clock;
		break;
	case MSR_KVM_SYSTEM_TIME:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE))
			return 1;

		msr_info->data = vcpu->arch.time;
		break;
	case MSR_KVM_SYSTEM_TIME_NEW:
		if (!guest_pv_has(vcpu, KVM_FEATURE_CLOCKSOURCE2))
			return 1;

		msr_info->data = vcpu->arch.time;
		break;
	case MSR_KVM_ASYNC_PF_EN:
		if (!guest_pv_has(vcpu, KVM_FEATURE_ASYNC_PF))
			return 1;

		msr_info->data = vcpu->arch.apf.msr_en_val;
		break;
	case MSR_KVM_ASYNC_PF_INT:
		if (!guest_pv_has(vcpu, KVM_FEATURE_ASYNC_PF_INT))
			return 1;

		msr_info->data = vcpu->arch.apf.msr_int_val;
		break;
	case MSR_KVM_ASYNC_PF_ACK:
		if (!guest_pv_has(vcpu, KVM_FEATURE_ASYNC_PF_INT))
			return 1;

		msr_info->data = 0;
		break;
	case MSR_KVM_STEAL_TIME:
		if (!guest_pv_has(vcpu, KVM_FEATURE_STEAL_TIME))
			return 1;

		msr_info->data = vcpu->arch.st.msr_val;
		break;
	case MSR_KVM_PV_EOI_EN:
		if (!guest_pv_has(vcpu, KVM_FEATURE_PV_EOI))
			return 1;

		msr_info->data = vcpu->arch.pv_eoi.msr_val;
		break;
	case MSR_KVM_POLL_CONTROL:
		if (!guest_pv_has(vcpu, KVM_FEATURE_POLL_CONTROL))
			return 1;

		msr_info->data = vcpu->arch.msr_kvm_poll_control;
		break;
	case MSR_IA32_P5_MC_ADDR:
	case MSR_IA32_P5_MC_TYPE:
	case MSR_IA32_MCG_CAP:
	case MSR_IA32_MCG_CTL:
	case MSR_IA32_MCG_STATUS:
	case MSR_IA32_MC0_CTL ... MSR_IA32_MCx_CTL(KVM_MAX_MCE_BANKS) - 1:
	case MSR_IA32_MC0_CTL2 ... MSR_IA32_MCx_CTL2(KVM_MAX_MCE_BANKS) - 1:
		return get_msr_mce(vcpu, msr_info->index, &msr_info->data,
				   msr_info->host_initiated);
#endif
	case MSR_IA32_XSS:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_XSAVES))
			return 1;
		msr_info->data = vcpu->arch.ia32_xss;
		break;
#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
	case MSR_K7_CLK_CTL:
		/*
		 * Provide expected ramp-up count for K7. All other
		 * are set to zero, indicating minimum divisors for
		 * every field.
		 *
		 * This prevents guest kernels on AMD host with CPU
		 * type 6, model 8 and higher from exploding due to
		 * the rdmsr failing.
		 */
		msr_info->data = 0x20000000;
		break;
#ifdef CONFIG_KVM_HYPERV
	case HV_X64_MSR_GUEST_OS_ID ... HV_X64_MSR_SINT15:
	case HV_X64_MSR_SYNDBG_CONTROL ... HV_X64_MSR_SYNDBG_PENDING_BUFFER:
	case HV_X64_MSR_SYNDBG_OPTIONS:
	case HV_X64_MSR_CRASH_P0 ... HV_X64_MSR_CRASH_P4:
	case HV_X64_MSR_CRASH_CTL:
	case HV_X64_MSR_STIMER0_CONFIG ... HV_X64_MSR_STIMER3_COUNT:
	case HV_X64_MSR_REENLIGHTENMENT_CONTROL:
	case HV_X64_MSR_TSC_EMULATION_CONTROL:
	case HV_X64_MSR_TSC_EMULATION_STATUS:
	case HV_X64_MSR_TSC_INVARIANT_CONTROL:
		return kvm_hv_get_msr_common(vcpu,
					     msr_info->index, &msr_info->data,
					     msr_info->host_initiated);
#endif
	case MSR_IA32_BBL_CR_CTL3:
		/* This legacy MSR exists but isn't fully documented in current
		 * silicon.  It is however accessed by winxp in very narrow
		 * scenarios where it sets bit #19, itself documented as
		 * a "reserved" bit.  Best effort attempt to source coherent
		 * read data here should the balance of the register be
		 * interpreted by the guest:
		 *
		 * L2 cache control register 3: 64GB range, 256KB size,
		 * enabled, latency 0x1, configured
		 */
		msr_info->data = 0xbe702111;
		break;
	case MSR_AMD64_OSVW_ID_LENGTH:
		if (!guest_cpuid_has(vcpu, X86_FEATURE_OSVW))
			return 1;
		msr_info->data = vcpu->arch.osvw.length;
		break;
	case MSR_AMD64_OSVW_STATUS:
		if (!guest_cpuid_has(vcpu, X86_FEATURE_OSVW))
			return 1;
		msr_info->data = vcpu->arch.osvw.status;
		break;
	case MSR_PLATFORM_INFO:
		if (!msr_info->host_initiated &&
		    !vcpu->kvm->arch.guest_can_read_msr_platform_info)
			return 1;
		msr_info->data = vcpu->arch.msr_platform_info;
		break;
	case MSR_MISC_FEATURES_ENABLES:
		msr_info->data = vcpu->arch.msr_misc_features_enables;
		break;
	case MSR_K7_HWCR:
		msr_info->data = vcpu->arch.msr_hwcr;
		break;
#ifdef CONFIG_X86_64
	case MSR_IA32_XFD:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_XFD))
			return 1;

		msr_info->data = vcpu->arch.guest_fpu.fpstate->xfd;
		break;
	case MSR_IA32_XFD_ERR:
		if (!msr_info->host_initiated &&
		    !guest_cpuid_has(vcpu, X86_FEATURE_XFD))
			return 1;

		msr_info->data = vcpu->arch.guest_fpu.xfd_err;
		break;
#endif
#endif
	default:
#ifndef __PKVM_HYP__ /* FIXME: Leave to the host to emulate */
		if (kvm_pmu_is_valid_msr(vcpu, msr_info->index))
			return kvm_pmu_get_msr(vcpu, msr_info);
#endif

		return KVM_MSR_RET_UNSUPPORTED;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_get_msr_common);

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

int kvm_skip_emulated_instruction(struct kvm_vcpu *vcpu)
{
#ifndef __PKVM_HYP__
	unsigned long rflags = kvm_x86_call(get_rflags)(vcpu);
#endif
	int r;

	r = kvm_x86_call(skip_emulated_instruction)(vcpu);
	if (unlikely(!r))
		return 0;

#ifndef __PKVM_HYP__ /* No PMU and guest_debug support in the pkvm hypervisor */
	kvm_pmu_trigger_event(vcpu, kvm_pmu_eventsel.INSTRUCTIONS_RETIRED);

	/*
	 * rflags is the old, "raw" value of the flags.  The new value has
	 * not been saved yet.
	 *
	 * This is correct even for TF set by the guest, because "the
	 * processor will not generate this exception after the instruction
	 * that sets the TF flag".
	 */
	if (unlikely(rflags & X86_EFLAGS_TF))
		r = kvm_vcpu_do_singlestep(vcpu);
#endif
	return r;
}
EXPORT_SYMBOL_GPL(kvm_skip_emulated_instruction);

int kvm_emulate_instruction(struct kvm_vcpu *vcpu, int emulation_type)
{
#ifdef __PKVM_HYP__
	return 0;
#else
	return x86_emulate_instruction(vcpu, 0, emulation_type, NULL, 0);
#endif
}
EXPORT_SYMBOL_GPL(kvm_emulate_instruction);

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

int kvm_check_nested_events(struct kvm_vcpu *vcpu)
{
#ifdef __PKVM_HYP__
	return 0;
#else
	if (kvm_test_request(KVM_REQ_TRIPLE_FAULT, vcpu)) {
		kvm_x86_ops.nested_ops->triple_fault(vcpu);
		return 1;
	}

	return kvm_x86_ops.nested_ops->check_events(vcpu);
#endif
}

static void kvm_inject_exception(struct kvm_vcpu *vcpu)
{
	/*
	 * Suppress the error code if the vCPU is in Real Mode, as Real Mode
	 * exceptions don't report error codes.  The presence of an error code
	 * is carried with the exception and only stripped when the exception
	 * is injected as intercepted #PF VM-Exits for AMD's Paged Real Mode do
	 * report an error code despite the CPU being in Real Mode.
	 */
	vcpu->arch.exception.has_error_code &= is_protmode(vcpu);

	trace_kvm_inj_exception(vcpu->arch.exception.vector,
				vcpu->arch.exception.has_error_code,
				vcpu->arch.exception.error_code,
				vcpu->arch.exception.injected);

	kvm_x86_call(inject_exception)(vcpu);
}

/*
 * Check for any event (interrupt or exception) that is ready to be injected,
 * and if there is at least one event, inject the event with the highest
 * priority.  This handles both "pending" events, i.e. events that have never
 * been injected into the guest, and "injected" events, i.e. events that were
 * injected as part of a previous VM-Enter, but weren't successfully delivered
 * and need to be re-injected.
 *
 * Note, this is not guaranteed to be invoked on a guest instruction boundary,
 * i.e. doesn't guarantee that there's an event window in the guest.  KVM must
 * be able to inject exceptions in the "middle" of an instruction, and so must
 * also be able to re-inject NMIs and IRQs in the middle of an instruction.
 * I.e. for exceptions and re-injected events, NOT invoking this on instruction
 * boundaries is necessary and correct.
 *
 * For simplicity, KVM uses a single path to inject all events (except events
 * that are injected directly from L1 to L2) and doesn't explicitly track
 * instruction boundaries for asynchronous events.  However, because VM-Exits
 * that can occur during instruction execution typically result in KVM skipping
 * the instruction or injecting an exception, e.g. instruction and exception
 * intercepts, and because pending exceptions have higher priority than pending
 * interrupts, KVM still honors instruction boundaries in most scenarios.
 *
 * But, if a VM-Exit occurs during instruction execution, and KVM does NOT skip
 * the instruction or inject an exception, then KVM can incorrecty inject a new
 * asynchronous event if the event became pending after the CPU fetched the
 * instruction (in the guest).  E.g. if a page fault (#PF, #NPF, EPT violation)
 * occurs and is resolved by KVM, a coincident NMI, SMI, IRQ, etc... can be
 * injected on the restarted instruction instead of being deferred until the
 * instruction completes.
 *
 * In practice, this virtualization hole is unlikely to be observed by the
 * guest, and even less likely to cause functional problems.  To detect the
 * hole, the guest would have to trigger an event on a side effect of an early
 * phase of instruction execution, e.g. on the instruction fetch from memory.
 * And for it to be a functional problem, the guest would need to depend on the
 * ordering between that side effect, the instruction completing, _and_ the
 * delivery of the asynchronous event.
 */
static int kvm_check_and_inject_events(struct kvm_vcpu *vcpu,
				       bool *req_immediate_exit)
{
	bool can_inject;
	int r;

	/*
	 * Process nested events first, as nested VM-Exit supersedes event
	 * re-injection.  If there's an event queued for re-injection, it will
	 * be saved into the appropriate vmc{b,s}12 fields on nested VM-Exit.
	 */
	if (is_guest_mode(vcpu))
		r = kvm_check_nested_events(vcpu);
	else
		r = 0;

	/*
	 * Re-inject exceptions and events *especially* if immediate entry+exit
	 * to/from L2 is needed, as any event that has already been injected
	 * into L2 needs to complete its lifecycle before injecting a new event.
	 *
	 * Don't re-inject an NMI or interrupt if there is a pending exception.
	 * This collision arises if an exception occurred while vectoring the
	 * injected event, KVM intercepted said exception, and KVM ultimately
	 * determined the fault belongs to the guest and queues the exception
	 * for injection back into the guest.
	 *
	 * "Injected" interrupts can also collide with pending exceptions if
	 * userspace ignores the "ready for injection" flag and blindly queues
	 * an interrupt.  In that case, prioritizing the exception is correct,
	 * as the exception "occurred" before the exit to userspace.  Trap-like
	 * exceptions, e.g. most #DBs, have higher priority than interrupts.
	 * And while fault-like exceptions, e.g. #GP and #PF, are the lowest
	 * priority, they're only generated (pended) during instruction
	 * execution, and interrupts are recognized at instruction boundaries.
	 * Thus a pending fault-like exception means the fault occurred on the
	 * *previous* instruction and must be serviced prior to recognizing any
	 * new events in order to fully complete the previous instruction.
	 */
	if (vcpu->arch.exception.injected)
		kvm_inject_exception(vcpu);
	else if (kvm_is_exception_pending(vcpu))
		; /* see above */
	else if (vcpu->arch.nmi_injected)
		kvm_x86_call(inject_nmi)(vcpu);
	else if (vcpu->arch.interrupt.injected)
		kvm_x86_call(inject_irq)(vcpu, true);

	/*
	 * Exceptions that morph to VM-Exits are handled above, and pending
	 * exceptions on top of injected exceptions that do not VM-Exit should
	 * either morph to #DF or, sadly, override the injected exception.
	 */
	WARN_ON_ONCE(vcpu->arch.exception.injected &&
		     vcpu->arch.exception.pending);

	/*
	 * Bail if immediate entry+exit to/from the guest is needed to complete
	 * nested VM-Enter or event re-injection so that a different pending
	 * event can be serviced (or if KVM needs to exit to userspace).
	 *
	 * Otherwise, continue processing events even if VM-Exit occurred.  The
	 * VM-Exit will have cleared exceptions that were meant for L2, but
	 * there may now be events that can be injected into L1.
	 */
	if (r < 0)
		goto out;

	/*
	 * A pending exception VM-Exit should either result in nested VM-Exit
	 * or force an immediate re-entry and exit to/from L2, and exception
	 * VM-Exits cannot be injected (flag should _never_ be set).
	 */
	WARN_ON_ONCE(vcpu->arch.exception_vmexit.injected ||
		     vcpu->arch.exception_vmexit.pending);

	/*
	 * New events, other than exceptions, cannot be injected if KVM needs
	 * to re-inject a previous event.  See above comments on re-injecting
	 * for why pending exceptions get priority.
	 */
	can_inject = !kvm_event_needs_reinjection(vcpu);

	if (vcpu->arch.exception.pending) {
		/*
		 * Fault-class exceptions, except #DBs, set RF=1 in the RFLAGS
		 * value pushed on the stack.  Trap-like exception and all #DBs
		 * leave RF as-is (KVM follows Intel's behavior in this regard;
		 * AMD states that code breakpoint #DBs excplitly clear RF=0).
		 *
		 * Note, most versions of Intel's SDM and AMD's APM incorrectly
		 * describe the behavior of General Detect #DBs, which are
		 * fault-like.  They do _not_ set RF, a la code breakpoints.
		 */
		if (exception_type(vcpu->arch.exception.vector) == EXCPT_FAULT)
			__kvm_set_rflags(vcpu, kvm_get_rflags(vcpu) |
					     X86_EFLAGS_RF);

		if (vcpu->arch.exception.vector == DB_VECTOR) {
			kvm_deliver_exception_payload(vcpu, &vcpu->arch.exception);
			if (vcpu->arch.dr7 & DR7_GD) {
				vcpu->arch.dr7 &= ~DR7_GD;
				kvm_update_dr7(vcpu);
			}
		}

		kvm_inject_exception(vcpu);

		vcpu->arch.exception.pending = false;
		vcpu->arch.exception.injected = true;

		can_inject = false;
	}

	/* Don't inject interrupts if the user asked to avoid doing so */
	if (vcpu->guest_debug & KVM_GUESTDBG_BLOCKIRQ)
		return 0;

	/*
	 * The host will use the PV interfaces to check and inject the
	 * smi/nmi/interrupt, and the pkvm hypervisor just needs to check if
	 * needs to re-inject which is done at the beginning.
	 */
#ifndef __PKVM_HYP__
	/*
	 * Finally, inject interrupt events.  If an event cannot be injected
	 * due to architectural conditions (e.g. IF=0) a window-open exit
	 * will re-request KVM_REQ_EVENT.  Sometimes however an event is pending
	 * and can architecturally be injected, but we cannot do it right now:
	 * an interrupt could have arrived just now and we have to inject it
	 * as a vmexit, or there could already an event in the queue, which is
	 * indicated by can_inject.  In that case we request an immediate exit
	 * in order to make progress and get back here for another iteration.
	 * The kvm_x86_ops hooks communicate this by returning -EBUSY.
	 */
#ifdef CONFIG_KVM_SMM
	if (vcpu->arch.smi_pending) {
		r = can_inject ? kvm_x86_call(smi_allowed)(vcpu, true) :
				 -EBUSY;
		if (r < 0)
			goto out;
		if (r) {
			vcpu->arch.smi_pending = false;
			++vcpu->arch.smi_count;
			enter_smm(vcpu);
			can_inject = false;
		} else
			kvm_x86_call(enable_smi_window)(vcpu);
	}
#endif

	if (vcpu->arch.nmi_pending) {
		r = can_inject ? kvm_x86_call(nmi_allowed)(vcpu, true) :
				 -EBUSY;
		if (r < 0)
			goto out;
		if (r) {
			--vcpu->arch.nmi_pending;
			vcpu->arch.nmi_injected = true;
			kvm_x86_call(inject_nmi)(vcpu);
			can_inject = false;
			WARN_ON(kvm_x86_call(nmi_allowed)(vcpu, true) < 0);
		}
		if (vcpu->arch.nmi_pending)
			kvm_x86_call(enable_nmi_window)(vcpu);
	}

	if (kvm_cpu_has_injectable_intr(vcpu)) {
		r = can_inject ? kvm_x86_call(interrupt_allowed)(vcpu, true) :
				 -EBUSY;
		if (r < 0)
			goto out;
		if (r) {
			int irq = kvm_cpu_get_interrupt(vcpu);

			if (!WARN_ON_ONCE(irq == -1)) {
				kvm_queue_interrupt(vcpu, irq, false);
				kvm_x86_call(inject_irq)(vcpu, false);
				WARN_ON(kvm_x86_call(interrupt_allowed)(vcpu, true) < 0);
			}
		}
		if (kvm_cpu_has_injectable_intr(vcpu))
			kvm_x86_call(enable_irq_window)(vcpu);
	}
#endif

	if (is_guest_mode(vcpu) &&
	    kvm_x86_ops.nested_ops->has_events &&
	    kvm_x86_ops.nested_ops->has_events(vcpu, true))
		*req_immediate_exit = true;

	/*
	 * KVM must never queue a new exception while injecting an event; KVM
	 * is done emulating and should only propagate the to-be-injected event
	 * to the VMCS/VMCB.  Queueing a new exception can put the vCPU into an
	 * infinite loop as KVM will bail from VM-Enter to inject the pending
	 * exception and start the cycle all over.
	 *
	 * Exempt triple faults as they have special handling and won't put the
	 * vCPU into an infinite loop.  Triple fault can be queued when running
	 * VMX without unrestricted guest, as that requires KVM to emulate Real
	 * Mode events (see kvm_inject_realmode_interrupt()).
	 */
	WARN_ON_ONCE(vcpu->arch.exception.pending ||
		     vcpu->arch.exception_vmexit.pending);
	return 0;

out:
	if (r == -EBUSY) {
		*req_immediate_exit = true;
		r = 0;
	}
	return r;
}

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

	vcpu->arch.pat = MSR_IA32_CR_PAT_DEFAULT;

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

void kvm_vcpu_reset(struct kvm_vcpu *vcpu, bool init_event)
{
	struct kvm_cpuid_entry2 *cpuid_0x1;
	unsigned long old_cr0 = kvm_read_cr0(vcpu);
	unsigned long new_cr0;

	/*
	 * Several of the "set" flows, e.g. ->set_cr0(), read other registers
	 * to handle side effects.  RESET emulation hits those flows and relies
	 * on emulated/virtualized registers, including those that are loaded
	 * into hardware, to be zeroed at vCPU creation.  Use CRs as a sentinel
	 * to detect improper or missing initialization.
	 */
	WARN_ON_ONCE(!init_event &&
		     (old_cr0 || kvm_read_cr3(vcpu) || kvm_read_cr4(vcpu)));

	/*
	 * SVM doesn't unconditionally VM-Exit on INIT and SHUTDOWN, thus it's
	 * possible to INIT the vCPU while L2 is active.  Force the vCPU back
	 * into L1 as EFER.SVME is cleared on INIT (along with all other EFER
	 * bits), i.e. virtualization is disabled.
	 */
	if (is_guest_mode(vcpu))
		kvm_leave_nested(vcpu);

#ifndef __PKVM_HYP__
	kvm_lapic_reset(vcpu, init_event);
#endif
	WARN_ON_ONCE(is_guest_mode(vcpu) || is_smm(vcpu));
	vcpu->arch.hflags = 0;

	vcpu->arch.smi_pending = 0;
	vcpu->arch.smi_count = 0;
	atomic_set(&vcpu->arch.nmi_queued, 0);
	vcpu->arch.nmi_pending = 0;
	vcpu->arch.nmi_injected = false;
	kvm_clear_interrupt_queue(vcpu);
	kvm_clear_exception_queue(vcpu);

	memset(vcpu->arch.db, 0, sizeof(vcpu->arch.db));
	kvm_update_dr0123(vcpu);
	vcpu->arch.dr6 = DR6_ACTIVE_LOW;
	vcpu->arch.dr7 = DR7_FIXED_1;
	kvm_update_dr7(vcpu);

	vcpu->arch.cr2 = 0;

	kvm_make_request(KVM_REQ_EVENT, vcpu);
	vcpu->arch.apf.msr_en_val = 0;
	vcpu->arch.apf.msr_int_val = 0;
	vcpu->arch.st.msr_val = 0;

	kvmclock_reset(vcpu);

	kvm_clear_async_pf_completion_queue(vcpu);
	kvm_async_pf_hash_reset(vcpu);
	vcpu->arch.apf.halted = false;

	/*
	 * FIXME: As the guest fpu is still managed by the host and the pkvm
	 * hypervisor doesn't have valid fpstate in its vcpu->arch.guest_fpu,
	 * reset the fpstate for MPX is done by the host when runs its
	 * kvm_vcpu_reset. To add fpu isolation, revisit to see how to do this
	 * in the pkvm hypervisor.
	 */
#ifndef __PKVM_HYP__
	if (vcpu->arch.guest_fpu.fpstate && kvm_mpx_supported()) {
		struct fpstate *fpstate = vcpu->arch.guest_fpu.fpstate;

		/*
		 * All paths that lead to INIT are required to load the guest's
		 * FPU state (because most paths are buried in KVM_RUN).
		 */
		if (init_event)
			kvm_put_guest_fpu(vcpu);

		fpstate_clear_xstate_component(fpstate, XFEATURE_BNDREGS);
		fpstate_clear_xstate_component(fpstate, XFEATURE_BNDCSR);

		if (init_event)
			kvm_load_guest_fpu(vcpu);
	}
#endif

	if (!init_event) {
		vcpu->arch.smbase = 0x30000;

		vcpu->arch.msr_misc_features_enables = 0;
		vcpu->arch.ia32_misc_enable_msr = MSR_IA32_MISC_ENABLE_PEBS_UNAVAIL |
						  MSR_IA32_MISC_ENABLE_BTS_UNAVAIL;

		__kvm_set_xcr(vcpu, 0, XFEATURE_MASK_FP);
		__kvm_set_msr(vcpu, MSR_IA32_XSS, 0, true);
	}

	/* All GPRs except RDX (handled below) are zeroed on RESET/INIT. */
	memset(vcpu->arch.regs, 0, sizeof(vcpu->arch.regs));
	kvm_register_mark_dirty(vcpu, VCPU_REGS_RSP);

	/*
	 * Fall back to KVM's default Family/Model/Stepping of 0x600 (P6/Athlon)
	 * if no CPUID match is found.  Note, it's impossible to get a match at
	 * RESET since KVM emulates RESET before exposing the vCPU to userspace,
	 * i.e. it's impossible for kvm_find_cpuid_entry() to find a valid entry
	 * on RESET.  But, go through the motions in case that's ever remedied.
	 */
	cpuid_0x1 = kvm_find_cpuid_entry(vcpu, 1);
	kvm_rdx_write(vcpu, cpuid_0x1 ? cpuid_0x1->eax : 0x600);

	kvm_x86_call(vcpu_reset)(vcpu, init_event);

	kvm_set_rflags(vcpu, X86_EFLAGS_FIXED);
	kvm_rip_write(vcpu, 0xfff0);

	vcpu->arch.cr3 = 0;
	kvm_register_mark_dirty(vcpu, VCPU_EXREG_CR3);

	/*
	 * CR0.CD/NW are set on RESET, preserved on INIT.  Note, some versions
	 * of Intel's SDM list CD/NW as being set on INIT, but they contradict
	 * (or qualify) that with a footnote stating that CD/NW are preserved.
	 */
	new_cr0 = X86_CR0_ET;
	if (init_event)
		new_cr0 |= (old_cr0 & (X86_CR0_NW | X86_CR0_CD));
	else
		new_cr0 |= X86_CR0_NW | X86_CR0_CD;

	kvm_x86_call(set_cr0)(vcpu, new_cr0);
	kvm_x86_call(set_cr4)(vcpu, 0);
	kvm_x86_call(set_efer)(vcpu, 0);
	kvm_x86_call(update_exception_bitmap)(vcpu);

	/*
	 * On the standard CR0/CR4/EFER modification paths, there are several
	 * complex conditions determining whether the MMU has to be reset and/or
	 * which PCIDs have to be flushed.  However, CR0.WP and the paging-related
	 * bits in CR4 and EFER are irrelevant if CR0.PG was '0'; and a reset+flush
	 * is needed anyway if CR0.PG was '1' (which can only happen for INIT, as
	 * CR0 will be '0' prior to RESET).  So we only need to check CR0.PG here.
	 */
	if (old_cr0 & X86_CR0_PG) {
		kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, vcpu);
#ifndef __PKVM_HYP__
		/* FIXME: How to handle this with PV EPT in the pkvm hypervisor? */
		kvm_mmu_reset_context(vcpu);
#endif
	}

	/*
	 * Intel's SDM states that all TLB entries are flushed on INIT.  AMD's
	 * APM states the TLBs are untouched by INIT, but it also states that
	 * the TLBs are flushed on "External initialization of the processor."
	 * Flush the guest TLB regardless of vendor, there is no meaningful
	 * benefit in relying on the guest to flush the TLB immediately after
	 * INIT.  A spurious TLB flush is benign and likely negligible from a
	 * performance perspective.
	 */
	if (init_event)
		kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, vcpu);
}
EXPORT_SYMBOL_GPL(kvm_vcpu_reset);

int kvm_arch_enable_virtualization_cpu(void)
{
#ifdef __PKVM_HYP__
	kvm_user_return_msr_cpu_online();

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

unsigned long kvm_get_rflags(struct kvm_vcpu *vcpu)
{
	unsigned long rflags;

	rflags = kvm_x86_call(get_rflags)(vcpu);
#ifndef __PKVM_HYP__ /* No guest_debug support in the pkvm hypervisor */
	if (vcpu->guest_debug & KVM_GUESTDBG_SINGLESTEP)
		rflags &= ~X86_EFLAGS_TF;
#endif
	return rflags;
}
EXPORT_SYMBOL_GPL(kvm_get_rflags);

static void __kvm_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags)
{
#ifndef __PKVM_HYP__ /* No guest_debug support in the pkvm hypervisor */
	if (vcpu->guest_debug & KVM_GUESTDBG_SINGLESTEP &&
	    kvm_is_linear_rip(vcpu, vcpu->arch.singlestep_rip))
		rflags |= X86_EFLAGS_TF;
#endif
	kvm_x86_call(set_rflags)(vcpu, rflags);
}

void kvm_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags)
{
	__kvm_set_rflags(vcpu, rflags);
	kvm_make_request(KVM_REQ_EVENT, vcpu);
}
EXPORT_SYMBOL_GPL(kvm_set_rflags);

int kvm_spec_ctrl_test_value(u64 value)
{
	/*
	 * test that setting IA32_SPEC_CTRL to given value
	 * is allowed by the host processor
	 */

	u64 saved_value;
	unsigned long flags;
	int ret = 0;

	local_irq_save(flags);

	/*
	 * TODO: the pkvm hypervisor doesn't have exception handlers thus cannot
	 * tell if the RDMSR/WRMSR has error or not.
	 */
	if (rdmsrl_safe(MSR_IA32_SPEC_CTRL, &saved_value))
		ret = 1;
	else if (wrmsrl_safe(MSR_IA32_SPEC_CTRL, value))
		ret = 1;
	else
		wrmsrl(MSR_IA32_SPEC_CTRL, saved_value);

	local_irq_restore(flags);

	return ret;
}
EXPORT_SYMBOL_GPL(kvm_spec_ctrl_test_value);

#ifdef __PKVM_HYP__
static void kvm_restore_user_return_msr(void)
{
	struct kvm_user_return_msrs *msrs = this_cpu_ptr(&user_return_msrs);
	struct kvm_user_return_msr_values *values;
	unsigned slot;

	for (slot = 0; slot < kvm_nr_uret_msrs; ++slot) {
		values = &msrs->values[slot];
		if (values->host != values->curr) {
			wrmsrl(kvm_uret_msrs_list[slot], values->host);
			values->curr = values->host;
		}
	}
}

unsigned long kvm_vcpu_enter_guest(struct kvm_vcpu *vcpu, bool force_immediate_exit)
{
	fastpath_t exit_fastpath;
	int ret;

	pkvm_reset_reqs_to_host(vcpu);

	if (kvm_x86_call(vcpu_pre_run)(vcpu) <= 0)
		return 0;

	vcpu->arch.last_vmentry_cpu = vcpu->cpu;

	/* TODO: Save the host VMM fpu and load the guest fpu */

	vcpu->arch.host_debugctl = get_debugctlmsr();

	for (;;) {
		bool req_immediate_exit = false;

		if (kvm_request_pending(vcpu)) {
			if (kvm_check_request(KVM_REQ_TLB_FLUSH, vcpu))
				kvm_vcpu_flush_tlb_all(vcpu);

			if (kvm_check_request(KVM_REQ_TLB_FLUSH_CURRENT, vcpu))
				kvm_vcpu_flush_tlb_current(vcpu);

			if (kvm_check_request(KVM_REQ_EVENT, vcpu))
				kvm_check_and_inject_events(vcpu, &req_immediate_exit);
		}

		kvm_x86_call(prepare_switch_to_guest)(vcpu);

		/*
		 * Make sure vcpu->mode is changed to IN_GUEST_MODE before
		 * running to mark this vcpu should be kicked for any new
		 * vcpu request.
		 */
		smp_store_mb(vcpu->mode, IN_GUEST_MODE);

		if (req_immediate_exit)
			kvm_make_request(KVM_REQ_EVENT, vcpu);
		else
			req_immediate_exit = force_immediate_exit;

		exit_fastpath = kvm_x86_call(vcpu_run)(vcpu, req_immediate_exit);

		/*
		 * Make sure vcpu->mode is changed to OUTSIDE_GUEST_MODE after
		 * vmexit to mark this vcpu no need to be kicked for any new
		 * vcpu request.
		 */
		smp_store_mb(vcpu->mode, OUTSIDE_GUEST_MODE);

		if (unlikely(exit_fastpath == EXIT_FASTPATH_REENTER_GUEST))
			continue;

		ret = kvm_x86_call(handle_exit)(vcpu, exit_fastpath);
		if (ret <= 0) {
			pkvm_make_req_to_host(HOST_HANDLE_EXIT, vcpu);
			break;
		}

		if (unlikely(force_immediate_exit) || pkvm_reqs_to_host(vcpu))
			break;
	}

	kvm_x86_call(prepare_switch_to_host)(vcpu);

	/* TODO: Restore the host VMM fpu and save the guest fpu */

	kvm_restore_user_return_msr();

	return pkvm_reqs_to_host(vcpu);
}
#endif
