// SPDX-License-Identifier: GPL-2.0
#include <linux/moduleparam.h>
#include <asm/perf_event.h>
#include <asm/kvm_pkvm.h>
#include <asm/fred.h>
#include <asm/pkvm.h>
#include "pkvm.h"
#include "pkvm_constants.h"

#include "x86_ops.h"
#include "vmx.h"
#include "nested.h"
#include "pmu.h"
#include "posted_intr.h"
#include <trace/events/ipi.h>
#include "trace.h"

static DEFINE_PER_CPU(union pkvm_pv_param, *pv_param);

#define get_this_pv_param(f)		(&per_cpu(pv_param, get_cpu())->f)
#define put_this_pv_param(ptr)		\
({					\
	memset(ptr, 0, sizeof(*ptr));	\
	ptr = NULL;			\
	put_cpu();			\
})

static void pkvm_mc_free_fn(void *addr, void *unused)
{
	free_page((unsigned long)addr);
}

static void *kvm_host_va(phys_addr_t phys)
{
	return __va(phys);
}

static void free_pkvm_memcache(struct pkvm_memcache *mc)
{
	__free_pkvm_memcache(mc, pkvm_mc_free_fn,
			     kvm_host_va, NULL);
}

static void free_pml_buffer(struct vcpu_vmx *vmx)
{
	if (vmx->pml_pg) {
		free_page((unsigned long)vmx->pml_pg);
		vmx->pml_pg = NULL;
	}
}

static void free_ve_info(struct vcpu_vmx *vmx)
{
	if (vmx->ve_info) {
		free_page((unsigned long)vmx->ve_info);
		vmx->ve_info = NULL;
	}
}

static void pkvm_free_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	if (!loaded_vmcs->vmcs)
		return;
	free_vmcs(loaded_vmcs->vmcs);
	loaded_vmcs->vmcs = NULL;
	if (loaded_vmcs->msr_bitmap)
		free_page((unsigned long)loaded_vmcs->msr_bitmap);
	WARN_ON(loaded_vmcs->shadow_vmcs != NULL);
}

static int pkvm_alloc_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	loaded_vmcs->vmcs = alloc_vmcs(false);
	if (!loaded_vmcs->vmcs)
		return -ENOMEM;

	loaded_vmcs->shadow_vmcs = NULL;
	loaded_vmcs->hv_timer_soft_disabled = false;
	loaded_vmcs->cpu = -1;

	if (cpu_has_vmx_msr_bitmap()) {
		loaded_vmcs->msr_bitmap = (unsigned long *)
				__get_free_page(GFP_KERNEL_ACCOUNT);
		if (!loaded_vmcs->msr_bitmap)
			goto out_vmcs;
	}

	memset(&loaded_vmcs->host_state, 0, sizeof(struct vmcs_host_state));
	memset(&loaded_vmcs->controls_shadow, 0,
		sizeof(struct vmcs_controls_shadow));

	return 0;

out_vmcs:
	pkvm_free_loaded_vmcs(loaded_vmcs);
	return -ENOMEM;
}

static void __pkvm_vcpu_unload(void *arg)
{
	struct kvm_vcpu *vcpu = arg;
	struct vcpu_vmx *vmx;

	kvm_call_pkvm(vcpu_put, vcpu);

	vmx = to_vmx(vcpu);
	vmx->loaded_vmcs->cpu = -1;
}

static void pkvm_vcpu_unload(struct kvm_vcpu *vcpu)
{
	int cpu = to_vmx(vcpu)->loaded_vmcs->cpu;

	if (cpu != -1)
		smp_call_function_single(cpu, __pkvm_vcpu_unload, vcpu, 1);
}

static bool pkvm_segment_cache_test(struct vcpu_vmx *vmx, int seg, int field)
{
	u32 mask = 1 << (seg * SEG_FIELD_NR + field);

	if (!kvm_register_is_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS)) {
		kvm_register_mark_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS);
		vmx->segment_cache.bitmask = 0;
	}

	return vmx->segment_cache.bitmask & mask;
}

static void pkvm_segment_cache_set(struct vcpu_vmx *vmx, int seg, int field)
{
	u32 mask = 1 << (seg * SEG_FIELD_NR + field);

	if (!kvm_register_is_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS)) {
		kvm_register_mark_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS);
		vmx->segment_cache.bitmask = 0;
	}

	vmx->segment_cache.bitmask |= mask;
}

static void pkvm_cache_segment(struct vcpu_vmx *vmx, struct kvm_segment *var, int seg)
{
	struct kvm_save_segment *save = &vmx->segment_cache.seg[seg];

	save->selector = var->selector;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_SEL);

	save->base = var->base;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_BASE);

	save->limit = var->limit;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_LIMIT);

	save->ar = (var->unusable << 16) |
		      (var->g << 15)	 |
		      (var->db << 14)	 |
		      (var->l << 13)	 |
		      (var->avl << 12)	 |
		      (var->present << 7)	 |
		      (var->dpl << 5)	 |
		      (var->s << 4)	 |
		      var->type;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_AR);
}

static bool pkvm_hyp_emulated_msr(u32 msr)
{
	switch (msr) {
	case MSR_EFER:
#ifdef CONFIG_X86_64
	case MSR_FS_BASE:
	case MSR_GS_BASE:
	case MSR_KERNEL_GS_BASE:
#endif
	case MSR_IA32_SYSENTER_CS:
	case MSR_IA32_SYSENTER_EIP:
	case MSR_IA32_SYSENTER_ESP:
	case MSR_IA32_DEBUGCTLMSR:
	case MSR_IA32_BNDCFGS:
	case MSR_IA32_UMWAIT_CONTROL:
	case MSR_IA32_SPEC_CTRL:
	case MSR_IA32_TSX_CTRL:
	case MSR_IA32_CR_PAT:
	case MSR_IA32_MCG_EXT_CTL:
	case MSR_IA32_FEAT_CTL:
	case MSR_IA32_SGXLEPUBKEYHASH0 ... MSR_IA32_SGXLEPUBKEYHASH3:
	case MSR_IA32_XSS:
	case MSR_IA32_MISC_ENABLE:
		return true;
	default:
		break;
	}

	return false;
}

static fastpath_t pkvm_exit_handlers_fastpath(struct kvm_vcpu *vcpu)
{
	switch (to_vmx(vcpu)->exit_reason.basic) {
	case EXIT_REASON_MSR_WRITE:
		return handle_fastpath_set_msr_irqoff(vcpu);
	default:
		return EXIT_FASTPATH_NONE;
	}
}

static int handle_exception_nmi(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct kvm_run *kvm_run = vcpu->run;
	u32 intr_info, ex_no, error_code;
	u32 vect_info;

	vect_info = vmx->idt_vectoring_info;
	intr_info = vmx_get_intr_info(vcpu);

	/*
	 * Machine checks are handled by handle_exception_irqoff(), or by
	 * pkvm_vcpu_run() if a #MC occurs on VM-Entry.  NMIs are handled by
	 * pkvm_vcpu_run().
	 */
	if (is_machine_check(intr_info) || is_nmi(intr_info))
		return 1;

	if (pkvm_is_protected_vcpu(vcpu))
		return 1;

	if (is_invalid_opcode(intr_info))
		return handle_ud(vcpu);

	error_code = 0;
	if (intr_info & INTR_INFO_DELIVER_CODE_MASK)
		error_code = vmx->error_code;

	/*
	 * The #PF with PFEC.RSVD = 1 indicates the guest is accessing
	 * MMIO, it is better to report an internal error.
	 * See the comments in __pkvm_handle_exit.
	 */
	if ((vect_info & VECTORING_INFO_VALID_MASK) &&
	    !(is_page_fault(intr_info) && !(error_code & PFERR_RSVD_MASK))) {
		vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		vcpu->run->internal.suberror = KVM_INTERNAL_ERROR_SIMUL_EX;
		vcpu->run->internal.ndata = 4;
		vcpu->run->internal.data[0] = vect_info;
		vcpu->run->internal.data[1] = intr_info;
		vcpu->run->internal.data[2] = error_code;
		vcpu->run->internal.data[3] = vcpu->arch.last_vmentry_cpu;
		return 0;
	}

	ex_no = intr_info & INTR_INFO_VECTOR_MASK;
	switch (ex_no) {
	case AC_VECTOR:
		if (vmx_guest_inject_ac(vcpu)) {
			kvm_queue_exception_e(vcpu, AC_VECTOR, error_code);
			return 1;
		}

		/*
		 * Handle split lock. Depending on detection mode this will
		 * either warn and disable split lock detection for this
		 * task or force SIGBUS on it.
		 */
		if (handle_guest_split_lock(kvm_rip_read(vcpu)))
			return 1;
		fallthrough;
	default:
		pr_warn("pkvm_high: Unsupported exception_nmi: intr_info 0x%x\n", intr_info);
		kvm_run->exit_reason = KVM_EXIT_EXCEPTION;
		kvm_run->ex.exception = ex_no;
		kvm_run->ex.error_code = error_code;
		break;
	}

	return 0;
}

static __always_inline int handle_external_interrupt(struct kvm_vcpu *vcpu)
{
	++vcpu->stat.irq_exits;
	return 1;
}

static int handle_triple_fault(struct kvm_vcpu *vcpu)
{
	vcpu->run->exit_reason = KVM_EXIT_SHUTDOWN;
	vcpu->mmio_needed = 0;
	return 0;
}

static int handle_nmi_window(struct kvm_vcpu *vcpu)
{
	++vcpu->stat.nmi_window_exits;
	kvm_make_request(KVM_REQ_EVENT, vcpu);

	return 1;
}

static int pkvm_complete_fast_pio_out(struct kvm_vcpu *vcpu)
{
	vcpu->arch.pio.count = 0;

	return pkvm_is_protected_vcpu(vcpu) ? 1 : kvm_skip_emulated_instruction(vcpu);
}

static int pkvm_fast_pio_out(struct kvm_vcpu *vcpu, int size,
			    unsigned short port)
{
	struct x86_emulate_ctxt *ctxt = vcpu->arch.emulate_ctxt;
	unsigned long val = kvm_rax_read(vcpu);
	int ret;

	ret = ctxt->ops->pio_out_emulated(ctxt, size, port, &val, 1);
	if (ret)
		return ret;

	vcpu->arch.complete_userspace_io = pkvm_complete_fast_pio_out;
	return 0;
}

static int pkvm_complete_fast_pio_in(struct kvm_vcpu *vcpu)
{
	unsigned int count = vcpu->arch.pio.count;
	int size = vcpu->arch.pio.size;
	unsigned long val;

	/* We should only ever be called with arch.pio.count equal to 1 */
	BUG_ON(vcpu->arch.pio.count != 1);

	/* For size less than 4 we merge, else we zero extend */
	val = (vcpu->arch.pio.size < 4) ? kvm_rax_read(vcpu) : 0;
	memcpy(&val, vcpu->arch.pio_data, size * count);

	trace_kvm_pio(KVM_PIO_IN, vcpu->arch.pio.port, size, count, vcpu->arch.pio_data);
	vcpu->arch.pio.count = 0;
	kvm_rax_write(vcpu, val);

	return pkvm_is_protected_vcpu(vcpu) ? 1 : kvm_skip_emulated_instruction(vcpu);
}

static int pkvm_fast_pio_in(struct kvm_vcpu *vcpu, int size,
			    unsigned short port)
{
	struct x86_emulate_ctxt *ctxt = vcpu->arch.emulate_ctxt;
	unsigned long val;
	int ret;

	/* For size less than 4 we merge, else we zero extend */
	val = (size < 4) ? kvm_rax_read(vcpu) : 0;

	ret = ctxt->ops->pio_in_emulated(ctxt, size, port, &val, 1);
	if (ret) {
		kvm_rax_write(vcpu, val);
		return ret;
	}

	vcpu->arch.complete_userspace_io = pkvm_complete_fast_pio_in;
	return 0;
}

static int handle_io(struct kvm_vcpu *vcpu)
{
	unsigned long exit_qualification;
	int size, in, string;
	unsigned int port;
	int ret;

	exit_qualification = vmx_get_exit_qual(vcpu);
	string = (exit_qualification & 16) != 0;

	++vcpu->stat.io_exits;

	if (string)
		return kvm_emulate_instruction(vcpu, 0);

	port = exit_qualification >> 16;
	size = (exit_qualification & 7) + 1;
	in = (exit_qualification & 8) != 0;

	ret = in ? pkvm_fast_pio_in(vcpu, size, port) :
		   pkvm_fast_pio_out(vcpu, size, port);

	return pkvm_is_protected_vcpu(vcpu) ?
			ret : (ret && kvm_skip_emulated_instruction(vcpu));
}

static int handle_tpr_below_threshold(struct kvm_vcpu *vcpu)
{
	kvm_apic_update_ppr(vcpu);
	return 1;
}

static int handle_interrupt_window(struct kvm_vcpu *vcpu)
{
	kvm_make_request(KVM_REQ_EVENT, vcpu);

	++vcpu->stat.irq_window_exits;
	return 1;
}

static int handle_apic_eoi_induced(struct kvm_vcpu *vcpu)
{
	unsigned long exit_qualification = vmx_get_exit_qual(vcpu);
	int vector = exit_qualification & 0xff;

	/* EOI-induced VM exit is trap-like and thus no need to adjust IP */
	kvm_apic_set_eoi_accelerated(vcpu, vector);
	return 1;
}

static int handle_apic_write(struct kvm_vcpu *vcpu)
{
	unsigned long exit_qualification = vmx_get_exit_qual(vcpu);

	/*
	 * APIC-write VM-Exit is trap-like, KVM doesn't need to advance RIP and
	 * hardware has done any necessary aliasing, offset adjustments, etc...
	 * for the access.  I.e. the correct value has already been  written to
	 * the vAPIC page for the correct 16-byte chunk.  KVM needs only to
	 * retrieve the register value and emulate the access.
	 */
	u32 offset = exit_qualification & 0xff0;

	kvm_apic_write_nodecode(vcpu, offset);
	return 1;
}

static int handle_machine_check(struct kvm_vcpu *vcpu)
{
	/* handled by pkvm_vcpu_run() */
	return 1;
}

static int handle_ept_violation(struct kvm_vcpu *vcpu)
{
	unsigned long exit_qualification;
	gpa_t gpa;
	u64 error_code;

	exit_qualification = vmx_get_exit_qual(vcpu);

	gpa = to_vmx(vcpu)->exit_gpa;
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

	return kvm_mmu_page_fault(vcpu, gpa, error_code, NULL, 0);
}

static int pkvm_check_emulate_instruction(struct kvm_vcpu *vcpu, int emul_type,
					  void *insn, int insn_len);

static int handle_ept_misconfig(struct kvm_vcpu *vcpu)
{
	gpa_t gpa;

	if (pkvm_check_emulate_instruction(vcpu, EMULTYPE_PF, NULL, 0))
		return 1;

	gpa = to_vmx(vcpu)->exit_gpa;
	if (!kvm_io_bus_write(vcpu, KVM_FAST_MMIO_BUS, gpa, 0, NULL)) {
		trace_kvm_fast_mmio(gpa);
		return kvm_skip_emulated_instruction(vcpu);
	}

	return kvm_mmu_page_fault(vcpu, gpa, PFERR_RSVD_MASK, NULL, 0);
}

/*
 * Indicate a busy-waiting vcpu in spinlock. We do not enable the PAUSE
 * exiting, so only get here on cpu with PAUSE-Loop-Exiting.
 */
static int handle_pause(struct kvm_vcpu *vcpu)
{
	/*
	 * Intel sdm vol3 ch-25.1.3 says: The "PAUSE-loop exiting"
	 * VM-execution control is ignored if CPL > 0. OTOH, KVM
	 * never set PAUSE_EXITING and just set PLE if supported,
	 * so the vcpu must be CPL=0 if it gets a PAUSE exit.
	 */
	kvm_vcpu_on_spin(vcpu, true);

	return 1;
}

static int handle_bus_lock_vmexit(struct kvm_vcpu *vcpu)
{
	/*
	 * The bus_lock_detected flag is set when got the vmexit reason from the
	 * pkvm hypervisor. Nothing to do here.
	 */
	return 1;
}

static int handle_notify(struct kvm_vcpu *vcpu)
{
	unsigned long exit_qual = vmx_get_exit_qual(vcpu);
	bool context_invalid = exit_qual & NOTIFY_VM_CONTEXT_INVALID;

	++vcpu->stat.notify_window_exits;

	if (vcpu->kvm->arch.notify_vmexit_flags & KVM_X86_NOTIFY_VMEXIT_USER ||
	    context_invalid) {
		vcpu->run->exit_reason = KVM_EXIT_NOTIFY;
		vcpu->run->notify.flags = context_invalid ?
					  KVM_NOTIFY_CONTEXT_INVALID : 0;
		return 0;
	}

	return 1;
}

static int handle_halt(struct kvm_vcpu *vcpu)
{
	return kvm_emulate_halt_noskip(vcpu);
}

static void wbinvd_ipi(void *garbage)
{
	wbinvd();
}

static inline bool pkvm_has_vmx_wbinvd_exit(void)
{
	/* FIXME: Check with the pkvm hypervisor */
	return true;
}

static int handle_wbinvd(struct kvm_vcpu *vcpu)
{
	if (!kvm_arch_has_noncoherent_dma(vcpu->kvm))
		return 1;

	if (pkvm_has_vmx_wbinvd_exit()) {
		int cpu = get_cpu();

		cpumask_set_cpu(cpu, vcpu->arch.wbinvd_dirty_mask);
		on_each_cpu_mask(vcpu->arch.wbinvd_dirty_mask,
				wbinvd_ipi, NULL, 1);
		put_cpu();
		cpumask_clear(vcpu->arch.wbinvd_dirty_mask);
	} else
		wbinvd();

	return 1;
}

/*
 * The exit handlers return 1 if the exit was handled fully and guest execution
 * may resume.  Otherwise they set the kvm_run parameter to indicate what needs
 * to be done to userspace and return 0.
 */
static int (*pkvm_vmx_exit_handlers[])(struct kvm_vcpu *vcpu) = {
	[EXIT_REASON_EXCEPTION_NMI]           = handle_exception_nmi,
	[EXIT_REASON_EXTERNAL_INTERRUPT]      = handle_external_interrupt,
	[EXIT_REASON_TRIPLE_FAULT]            = handle_triple_fault,
	[EXIT_REASON_NMI_WINDOW]	      = handle_nmi_window,
	[EXIT_REASON_IO_INSTRUCTION]          = handle_io,
	[EXIT_REASON_MSR_READ]                = kvm_emulate_rdmsr,
	[EXIT_REASON_MSR_WRITE]               = kvm_emulate_wrmsr,
	[EXIT_REASON_INTERRUPT_WINDOW]        = handle_interrupt_window,
	[EXIT_REASON_HLT]                     = handle_halt,
	[EXIT_REASON_VMCALL]                  = kvm_emulate_hypercall,
	[EXIT_REASON_TPR_BELOW_THRESHOLD]     = handle_tpr_below_threshold,
	[EXIT_REASON_APIC_WRITE]              = handle_apic_write,
	[EXIT_REASON_EOI_INDUCED]             = handle_apic_eoi_induced,
	[EXIT_REASON_WBINVD]                  = handle_wbinvd,
	[EXIT_REASON_MCE_DURING_VMENTRY]      = handle_machine_check,
	[EXIT_REASON_EPT_VIOLATION]	      = handle_ept_violation,
	[EXIT_REASON_EPT_MISCONFIG]           = handle_ept_misconfig,
	[EXIT_REASON_PAUSE_INSTRUCTION]       = handle_pause,
	[EXIT_REASON_BUS_LOCK]                = handle_bus_lock_vmexit,
	[EXIT_REASON_NOTIFY]		      = handle_notify,
};

static const int pkvm_vmx_max_exit_handlers =
	ARRAY_SIZE(pkvm_vmx_exit_handlers);

static int __pkvm_handle_exit(struct kvm_vcpu *vcpu, fastpath_t exit_fastpath)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	union vmx_exit_reason exit_reason;
	u16 exit_handler_index;
	u32 vectoring_info;

	exit_reason = vmx->exit_reason;

	if (exit_reason.failed_vmentry) {
		vcpu->run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		vcpu->run->fail_entry.hardware_entry_failure_reason
			= exit_reason.full;
		vcpu->run->fail_entry.cpu = vcpu->arch.last_vmentry_cpu;
		return 0;
	}

	if (unlikely(vmx->fail)) {
		vcpu->run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		vcpu->run->fail_entry.hardware_entry_failure_reason
			= vmx->error_code;
		vcpu->run->fail_entry.cpu = vcpu->arch.last_vmentry_cpu;
		return 0;
	}

	vectoring_info = vmx->idt_vectoring_info;
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
		int ndata = 3;

		vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		vcpu->run->internal.suberror = KVM_INTERNAL_ERROR_DELIVERY_EV;
		vcpu->run->internal.data[0] = vectoring_info;
		vcpu->run->internal.data[1] = exit_reason.full;
		vcpu->run->internal.data[2] = vmx_get_exit_qual(vcpu);
		if (exit_reason.basic == EXIT_REASON_EPT_MISCONFIG)
			vcpu->run->internal.data[ndata++] = vmx->exit_gpa;
		vcpu->run->internal.data[ndata++] = vcpu->arch.last_vmentry_cpu;
		vcpu->run->internal.ndata = ndata;
		return 0;
	}

	if (exit_fastpath != EXIT_FASTPATH_NONE)
		return 1;

	if (exit_reason.basic >= pkvm_vmx_max_exit_handlers)
		goto unexpected_vmexit;
#ifdef CONFIG_MITIGATION_RETPOLINE
	if (exit_reason.basic == EXIT_REASON_MSR_WRITE)
		return kvm_emulate_wrmsr(vcpu);
	else if (exit_reason.basic == EXIT_REASON_INTERRUPT_WINDOW)
		return handle_interrupt_window(vcpu);
	else if (exit_reason.basic == EXIT_REASON_EXTERNAL_INTERRUPT)
		return handle_external_interrupt(vcpu);
	else if (exit_reason.basic == EXIT_REASON_HLT)
		return handle_halt(vcpu);
	else if (exit_reason.basic == EXIT_REASON_EPT_MISCONFIG)
		return handle_ept_misconfig(vcpu);
#endif

	exit_handler_index = array_index_nospec((u16)exit_reason.basic,
						pkvm_vmx_max_exit_handlers);
	if (!pkvm_vmx_exit_handlers[exit_handler_index])
		goto unexpected_vmexit;

	return pkvm_vmx_exit_handlers[exit_handler_index](vcpu);

unexpected_vmexit:
	vcpu_unimpl(vcpu, "vmx: unexpected exit reason 0x%x\n",
		    exit_reason.full);
	vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
	vcpu->run->internal.suberror =
			KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
	vcpu->run->internal.ndata = 2;
	vcpu->run->internal.data[0] = exit_reason.full;
	vcpu->run->internal.data[1] = vcpu->arch.last_vmentry_cpu;
	return 0;
}

static int pkvm_check_processor_compat(void)
{
	return kvm_call_pkvm(check_processor_compatibility);
}

static int pkvm_enable_virtualization_cpu(void)
{
	size_t size = sizeof(union pkvm_pv_param);
	void *page;
	int r;

	page = alloc_pages_exact(size, GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	/* TODO: share union pkvm_pv_param with pkvm */
	r = kvm_call_pkvm(enable_virtualization_cpu, __pa(page));
	if (r) {
		free_pages_exact(page, size);
		return r;
	}

	this_cpu_write(pv_param, page);
	intel_pt_handle_vmx(1);
	return 0;
}

static void pkvm_disable_virtualization_cpu(void)
{
	size_t size = sizeof(union pkvm_pv_param);
	void *page = this_cpu_read(pv_param);

	intel_pt_handle_vmx(0);
	kvm_call_pkvm(disable_virtualization_cpu);
	/* TODO: unshare union pkvm_pv_param with pkvm */
	free_pages_exact(page, size);
	this_cpu_write(pv_param, NULL);
}

static void pkvm_emergency_disable_virtualization_cpu(void) { /* TODO */ }

static bool pkvm_has_emulated_msr(struct kvm *kvm, u32 index)
{
	switch (index) {
	case MSR_IA32_SMBASE:
		/* The guest SMM mode is not supported. */
	case KVM_FIRST_EMULATED_VMX_MSR ... KVM_LAST_EMULATED_VMX_MSR:
		/* The guest VMX feature is not supported */
	case MSR_AMD64_VIRT_SPEC_CTRL:
	case MSR_AMD64_TSC_RATIO:
		/* This is AMD only.  */
		return false;
	default:
		/*
		 * The MSR not emulated by the pkvm hypervisor can be emualted
		 * by the host.
		 */
		return !pkvm_hyp_emulated_msr(index);
	}
}

static int pkvm_vm_init(struct kvm *kvm)
{
	struct kvm_protected_vm *pkvm = &kvm->arch.pkvm;
	size_t pkvm_vm_sz;
	void *pkvm_vm;
	int ret;

	ret = vmx_vm_init(kvm);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&pkvm->pinned_pages);
	pkvm->pvmfw_load_addr = PVMFW_INVALID_LOAD_ADDR;

	pkvm_vm_sz = PAGE_ALIGN(PKVM_SHADOW_VM_SIZE);
	pkvm_vm = alloc_pages_exact(pkvm_vm_sz, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vm)
		return -ENOMEM;

	/* TODO: share struct kvm_vmx with pkvm */

	ret = kvm_call_pkvm(vm_init, kvm, __pa(pkvm_vm));
	if (ret < 0)
		goto free_page;

	pkvm->pkvm_vm_handle = ret;

	if (pkvm_is_protected_vm(kvm))
		kvm->arch.has_protected_state = true;

	return 0;

free_page:
	free_pages_exact(pkvm_vm, pkvm_vm_sz);
	return ret;
}

static void pkvm_vm_destroy(struct kvm *kvm)
{
	struct kvm_protected_vm *pkvm = &kvm->arch.pkvm;
	struct kvm_pinned_page *ppage, *n;
	struct kvm_vcpu *vcpu;
	unsigned long i;
	int ret;

	/*
	 * Make sure each vcpu is unloaded in the pkvm hypervisor before destroy
	 * VM.
	 */
	kvm_for_each_vcpu(i, vcpu, kvm)
		pkvm_vcpu_unload(vcpu);

	ret = kvm_call_pkvm(vm_destroy, pkvm->pkvm_vm_handle);
	if (ret)
		return;

	/* TODO: unshare struct kvm_vmx with pkvm */

	free_pkvm_memcache(&pkvm->teardown_mc);

	list_for_each_entry_safe(ppage, n, &pkvm->pinned_pages, list) {
		list_del(&ppage->list);
		put_page(ppage->page);
		kfree(ppage);
	}

	vmx_vm_destroy(kvm);
}

static int pkvm_vcpu_create(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx;
	size_t pkvm_vcpu_sz;
	struct page *page;
	void *pkvm_vcpu;
	int ret;

	BUILD_BUG_ON(offsetof(struct vcpu_vmx, vcpu) != 0);
	vmx = to_vmx(vcpu);

	INIT_LIST_HEAD(&vmx->pi_wakeup_list);

	/*
	 * If PML is turned on, failure on enabling PML just results in failure
	 * of creating the vcpu, therefore we can simplify PML logic (by
	 * avoiding dealing with cases, such as enabling PML partially on vcpus
	 * for the guest), etc.
	 */
	if (enable_pml) {
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page)
			return -ENOMEM;
		vmx->pml_pg = page_to_virt(page);
	}

	ret = pkvm_alloc_loaded_vmcs(&vmx->vmcs01);
	if (ret < 0)
		goto free_pml;

	vmx->loaded_vmcs = &vmx->vmcs01;
	vmx->loaded_vmcs->cpu = -1;

	ret = -ENOMEM;

	if (vmcs_config.cpu_based_2nd_exec_ctrl & SECONDARY_EXEC_EPT_VIOLATION_VE) {
		BUILD_BUG_ON(sizeof(*vmx->ve_info) > PAGE_SIZE);

		/* ve_info must be page aligned. */
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page)
			goto free_vmcs;

		vmx->ve_info = page_to_virt(page);
	}

	pkvm_vcpu_sz = PAGE_ALIGN(PKVM_SHADOW_VCPU_STATE_SIZE);
	pkvm_vcpu = alloc_pages_exact(pkvm_vcpu_sz, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vcpu)
		goto free_ve;

	/* TODO: share struct vcpu_vmx with pkvm */

	ret = kvm_call_pkvm(vcpu_create, vcpu, __pa(pkvm_vcpu));
	if (ret < 0)
		goto free_pages;

	vcpu->arch.pkvm_vcpu_handle = ret;

	return 0;

free_pages:
	free_pages_exact(pkvm_vcpu, pkvm_vcpu_sz);
free_ve:
	free_ve_info(vmx);
free_vmcs:
	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
free_pml:
	free_pml_buffer(vmx);
	return ret;
}

static void pkvm_vcpu_free(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	/* TODO: unshare struct vcpu_vmx with pkvm */

	if (enable_pml)
		free_pml_buffer(vmx);
	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
	free_ve_info(vmx);
}

static void pkvm_vcpu_reset(struct kvm_vcpu *vcpu, bool init_event)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!vcpu->arch.guest_state_protected)
		kvm_call_pkvm(vcpu_reset, vcpu, init_event);

	/*
	 * The host response to inject interrupts to the guest. The pi_desc is
	 * the key structure for the host to inject interrupts via the posted
	 * interrupt mechanism. Its physical address is used for the
	 * POSTED_INTR_DESC_ADDR in the VMCS by the pkvm hypervisor. Initialize
	 * the pi_desc when reset vcpu.
	 */
	vmx->pi_desc.nv = POSTED_INTR_VECTOR;
	__pi_set_sn(&vmx->pi_desc);

	/*
	 * The CR0/CR4 guest-owned/rsvd bits are controlled by the pkvm
	 * hypervisor. The host VMM can assume all the bits in CR0/CR4 are owned
	 * by the guest.
	 */
	vcpu->arch.cr0_guest_owned_bits = ~0;
	vcpu->arch.cr4_guest_owned_bits = ~0;
	vcpu->arch.cr4_guest_rsvd_bits = 0;

	kvm_set_cr8(vcpu, 0);
	kvm_make_request(KVM_REQ_APIC_PAGE_RELOAD, vcpu);

	/* Enable x2apic by default */
	if (pkvm_is_protected_vcpu(vcpu)) {
		struct msr_data apic_base_msr;

		apic_base_msr.data = APIC_DEFAULT_PHYS_BASE |
				     LAPIC_MODE_X2APIC |
				     (kvm_vcpu_is_reset_bsp(vcpu) ? MSR_IA32_APICBASE_BSP : 0);
		apic_base_msr.host_initiated = true;

		kvm_set_apic_base(vcpu, &apic_base_msr);
	}
}

static void pkvm_prepare_switch_to_guest(struct kvm_vcpu *vcpu) {}

static void pkvm_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	bool already_loaded;

	already_loaded = vmx->loaded_vmcs->cpu == cpu;
	if (!already_loaded)
		pkvm_vcpu_unload(vcpu);

	kvm_call_pkvm(vcpu_load, vcpu, cpu);

	if (!already_loaded)
		vmx->loaded_vmcs->cpu = cpu;

	vmx_vcpu_pi_load(vcpu, cpu);
}

static void pkvm_vcpu_put(struct kvm_vcpu *vcpu)
{
	vmx_vcpu_pi_put(vcpu);
}

static void pkvm_update_exception_bitmap(struct kvm_vcpu *vcpu)
{
	/* TODO: Support for npVM */
}

static int pkvm_get_feature_msr(u32 msr, u64 *data)
{
	switch (msr) {
	case KVM_FIRST_EMULATED_VMX_MSR ... KVM_LAST_EMULATED_VMX_MSR:
		return 1;
	default:
		return KVM_MSR_RET_UNSUPPORTED;
	}
}

static int pkvm_get_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	/* Use PV interface to get the MSR emulated by the pkvm hypervisor */
	if (pkvm_hyp_emulated_msr(msr_info->index)) {
		if (!vcpu->arch.guest_state_protected) {
			struct msr_data *msr = get_this_pv_param(msr);
			int ret;

			*msr = *msr_info;
			ret = kvm_call_pkvm(get_msr, vcpu, msr);
			msr_info->data = msr->data;
			put_this_pv_param(msr);

			return ret;
		} else {
			return -EINVAL;
		}
	}

	/* Otherwise handle by the host VMM itself */
	return kvm_get_msr_common(vcpu, msr_info);
}

static int pkvm_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	int ret;

	/* Use PV interface to set the MSR emulated by the pkvm hypervisor */
	if (pkvm_hyp_emulated_msr(msr_info->index)) {
		if (!vcpu->arch.guest_state_protected) {
			struct msr_data *msr = get_this_pv_param(msr);
			int ret;

			*msr = *msr_info;
			ret = kvm_call_pkvm(set_msr, vcpu, msr);
			put_this_pv_param(msr);

			return ret;
		} else {
			return -EINVAL;
		}
	}

	/* Otherwise handle by the host VMM itself */
	ret = kvm_set_msr_common(vcpu, msr_info);
	if (ret)
		return ret;

	/*
	 * FIXME: The pkvm hypervisor will disable the write intercept for the
	 * XFD MSR. But as the FPU switching is done by the host, has to set the
	 * xfd_no_write_intercept here. Once the FPU switching can be done in
	 * the pkvm hypervisor, this can be removed.
	 */
	if (msr_info->index == MSR_IA32_XFD && msr_info->data)
		vcpu->arch.xfd_no_write_intercept = true;

	return 0;
}

static u64 pkvm_get_segment_base(struct kvm_vcpu *vcpu, int seg)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	ulong *p;

	if (vcpu->arch.guest_state_protected)
		return 0;

	p = &vmx->segment_cache.seg[seg].base;

	if (!pkvm_segment_cache_test(vmx, seg, SEG_FIELD_BASE)) {
		*p = kvm_call_pkvm(get_segment_base, vcpu, seg);
		pkvm_segment_cache_set(vmx, seg, SEG_FIELD_BASE);
	}

	return *p;
}

static void pkvm_get_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct kvm_save_segment *segment;
	u32 ar;

	if (vcpu->arch.guest_state_protected) {
		memset(var, 0, sizeof(*var));
		return;
	}

	if (!pkvm_segment_cache_test(vmx, seg, SEG_FIELD_SEL) ||
	    !pkvm_segment_cache_test(vmx, seg, SEG_FIELD_BASE) ||
	    !pkvm_segment_cache_test(vmx, seg, SEG_FIELD_LIMIT) ||
	    !pkvm_segment_cache_test(vmx, seg, SEG_FIELD_AR)) {
		struct kvm_segment *pkvm_var = get_this_pv_param(seg);

		kvm_call_pkvm(get_segment, vcpu, pkvm_var, seg);

		pkvm_cache_segment(vmx, pkvm_var, seg);

		put_this_pv_param(pkvm_var);
	}

	segment = &vmx->segment_cache.seg[seg];
	var->selector = segment->selector;
	var->base = segment->base;
	var->limit = segment->limit;
	ar = segment->ar;
	var->unusable = (ar >> 16) & 1;
	var->type = ar & 15;
	var->s = (ar >> 4) & 1;
	var->dpl = (ar >> 5) & 3;
	/*
	 * Some userspaces do not preserve unusable property. Since usable
	 * segment has to be present according to VMX spec we can use present
	 * property to amend userspace bug by making unusable segment always
	 * nonpresent. vmx_segment_access_rights() already marks nonpresent
	 * segment as unusable.
	 */
	var->present = !var->unusable;
	var->avl = (ar >> 12) & 1;
	var->l = (ar >> 13) & 1;
	var->db = (ar >> 14) & 1;
	var->g = (ar >> 15) & 1;
}

static void pkvm_set_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg)
{
	struct kvm_segment *pkvm_var;

	if (vcpu->arch.guest_state_protected)
		return;

	pkvm_var = get_this_pv_param(seg);
	*pkvm_var = *var;
	kvm_call_pkvm(set_segment, vcpu, pkvm_var, seg);
	put_this_pv_param(pkvm_var);
}

static int pkvm_get_cpl(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int seg = VCPU_SREG_SS;
	u32 ar;

	if (vcpu->arch.guest_state_protected ||
	    WARN_ON_ONCE(!pkvm_segment_cache_test(vmx, seg, SEG_FIELD_AR)))
		return 0;

	ar = vmx->segment_cache.seg[seg].ar;
	return VMX_AR_DPL(ar);
}

static void pkvm_get_cs_db_l_bits(struct kvm_vcpu *vcpu, int *db, int *l)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int seg = VCPU_SREG_CS;
	u32 ar;

	if (vcpu->arch.guest_state_protected ||
	    WARN_ON_ONCE(!pkvm_segment_cache_test(vmx, seg, SEG_FIELD_AR))) {
		*db = *l = 0;
		return;
	}

	ar = vmx->segment_cache.seg[seg].ar;
	*db = (ar >> 14) & 1;
	*l = (ar >> 13) & 1;
}

static bool pkvm_is_valid_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
	return true;
}

static void pkvm_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
	if (!vcpu->arch.guest_state_protected)
		kvm_call_pkvm(set_cr0, vcpu, cr0);

	vcpu->arch.cr0 = cr0;
}

static void pkvm_post_set_cr3(struct kvm_vcpu *vcpu, unsigned long cr3)
{
	if (vcpu->arch.guest_state_protected)
		return;

	kvm_call_pkvm(post_set_cr3, vcpu, cr3);
}

static bool pkvm_is_valid_cr4(struct kvm_vcpu *vcpu, unsigned long cr4)
{
	/* No VMX emulation in the pkvm hypervisor */
	if (cr4 & X86_CR4_VMXE)
		return false;

	return true;
}

static void pkvm_set_cr4(struct kvm_vcpu *vcpu, unsigned long cr4)
{
	if (!vcpu->arch.guest_state_protected)
		kvm_call_pkvm(set_cr4, vcpu, cr4);

	vcpu->arch.cr4 = cr4;
}

static int pkvm_set_efer(struct kvm_vcpu *vcpu, u64 efer)
{
	int ret = -EINVAL;

	if (!vcpu->arch.guest_state_protected)
		ret = kvm_call_pkvm(set_efer, vcpu, efer);

	vcpu->arch.efer = efer;
	return ret;
}

static void pkvm_access_idt_gdt(struct kvm_vcpu *vcpu, struct desc_ptr *dt,
				bool set, bool idt)
{
	struct desc_ptr *desc;

	if (vcpu->arch.guest_state_protected) {
		if (!set)
			memset(dt, 0, sizeof(*dt));
		return;
	}

	desc = get_this_pv_param(desc);

	if (set) {
		desc->size = dt->size;
		desc->address = dt->address;
		if (idt)
			kvm_call_pkvm(set_idt, vcpu, desc);
		else
			kvm_call_pkvm(set_gdt, vcpu, desc);
	} else {
		if (idt)
			kvm_call_pkvm(get_idt, vcpu, desc);
		else
			kvm_call_pkvm(get_gdt, vcpu, desc);
		dt->size = desc->size;
		dt->address = desc->address;
	}

	put_this_pv_param(desc);
}

static void pkvm_get_idt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, false, true);
}

static void pkvm_set_idt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, true, true);
}

static void pkvm_get_gdt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, false, false);
}

static void pkvm_set_gdt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, true, false);
}

static void pkvm_set_dr7(struct kvm_vcpu *vcpu, unsigned long val)
{
	if (vcpu->arch.guest_state_protected)
		return;

	kvm_call_pkvm(set_dr7, vcpu, val);
}

static void pkvm_sync_dirty_debug_regs(struct kvm_vcpu *vcpu) {}

static void pkvm_cache_reg(struct kvm_vcpu *vcpu, enum kvm_reg reg)
{
	kvm_register_mark_available(vcpu, reg);

	if (vcpu->arch.guest_state_protected)
		return;

	switch (reg) {
	case VCPU_REGS_RSP:
	case VCPU_REGS_RIP:
	case VCPU_EXREG_PDPTR:
	case VCPU_EXREG_CR0:
	case VCPU_EXREG_CR3:
	case VCPU_EXREG_CR4:
		kvm_call_pkvm(cache_reg, vcpu, reg);
		break;
	default:
		KVM_BUG_ON(1, vcpu->kvm);
		break;
	}
}

static unsigned long pkvm_get_rflags(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!kvm_register_is_available(vcpu, VCPU_EXREG_RFLAGS)) {
		kvm_register_mark_available(vcpu, VCPU_EXREG_RFLAGS);
		if (vcpu->arch.guest_state_protected)
			vmx->rflags = 0;
		else
			vmx->rflags = kvm_call_pkvm(get_rflags, vcpu);
	}

	return vmx->rflags;
}

static void pkvm_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags)
{
	kvm_register_mark_available(vcpu, VCPU_EXREG_RFLAGS);
	to_vmx(vcpu)->rflags = rflags;
	if (vcpu->arch.guest_state_protected)
		return;
	kvm_call_pkvm(set_rflags, vcpu, rflags);
}

static bool pkvm_get_if_flag(struct kvm_vcpu *vcpu)
{
	return pkvm_get_rflags(vcpu) & X86_EFLAGS_IF;
}

static void pkvm_flush_tlb_all(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(flush_tlb_all, vcpu);
}

static void pkvm_flush_tlb_current(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(flush_tlb_current, vcpu);
}

static void pkvm_flush_tlb_gva(struct kvm_vcpu *vcpu, gva_t addr)
{
	kvm_call_pkvm(flush_tlb_gva, vcpu, addr);
}

static void pkvm_flush_tlb_guest(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(flush_tlb_guest, vcpu);
}

void vmx_do_nmi_irqoff(void);

static fastpath_t pkvm_vcpu_run(struct kvm_vcpu *vcpu, bool force_immediate_exit)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long reqs_to_host;
	fastpath_t exit_fastpath;

	/* Record the guest's net vcpu time for enforced NMI injections. */
	if (unlikely(!enable_vnmi &&
		     vmx->loaded_vmcs->soft_vnmi_blocked))
		vmx->loaded_vmcs->entry_time = ktime_get();

	trace_kvm_entry(vcpu, force_immediate_exit);

	kvm_wait_lapic_expire(vcpu);

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

	vcpu->arch.nmi_injected = false;
	kvm_clear_exception_queue(vcpu);
	kvm_clear_interrupt_queue(vcpu);

	vmx->exit_reason.full = 0xdead;

	vcpu->arch.regs_avail &= ~VMX_REGS_LAZY_LOAD_SET;
	reqs_to_host = kvm_call_pkvm(vcpu_run, vcpu, force_immediate_exit);
	vcpu->arch.regs_dirty = 0;

	/*
	 * FIXME: The host still needs to pre-configure pVM's vcpu state for
	 * booting. Once the vcpu has started running, this will be dis-allowed
	 * by the pkvm hypervisor. So the guest_state_protected flag has to be
	 * set after the vcpu has started running. This is a temporary solution.
	 * Once the host doesn't need to do so, then the guest_state_protected
	 * can be enabled earlier.
	 */
	if (unlikely(vcpu->kvm->arch.has_protected_state &&
		     !vcpu->arch.guest_state_protected))
		vcpu->arch.guest_state_protected = true;

	if (unlikely(vmx->exit_reason.full == 0xdead)) {
		vmx->fail = 1;
		return EXIT_FASTPATH_NONE;
	}

	if ((u16)vmx->exit_reason.basic == EXIT_REASON_EXCEPTION_NMI &&
	    is_nmi(vmx_get_intr_info(vcpu))) {
		kvm_before_interrupt(vcpu, KVM_HANDLING_NMI);
		if (cpu_feature_enabled(X86_FEATURE_FRED))
			fred_entry_from_kvm(EVENT_TYPE_NMI, NMI_VECTOR);
		else
			vmx_do_nmi_irqoff();
		kvm_after_interrupt(vcpu);
	}

	guest_state_exit_irqoff();

	if (unlikely((u16)vmx->exit_reason.basic == EXIT_REASON_MCE_DURING_VMENTRY))
		kvm_machine_check();

	trace_kvm_exit(vcpu, KVM_ISA_VMX);

	if (unlikely(vmx->exit_reason.failed_vmentry))
		return EXIT_FASTPATH_NONE;

	if (unlikely(!enable_vnmi &&
		     vmx->loaded_vmcs->soft_vnmi_blocked))
		vmx->loaded_vmcs->vnmi_blocked_time +=
			ktime_to_ns(ktime_sub(ktime_get(),
					      vmx->loaded_vmcs->entry_time));

	if (vcpu->arch.exception.injected)
		kvm_make_request(KVM_REQ_EVENT, vcpu);

	exit_fastpath = EXIT_FASTPATH_EXIT_HANDLED;
	if (reqs_to_host) {
		if (test_and_clear_bit(HOST_HANDLE_EXIT, &reqs_to_host))
			exit_fastpath = EXIT_FASTPATH_NONE;

		if (test_and_clear_bit(HOST_RESET_MMU, &reqs_to_host))
			kvm_mmu_reset_context(vcpu);

		if (test_and_clear_bit(HOST_INIT_MMU, &reqs_to_host))
			kvm_init_mmu(vcpu);
	}

	if (exit_fastpath == EXIT_FASTPATH_EXIT_HANDLED)
		return exit_fastpath;

	return pkvm_exit_handlers_fastpath(vcpu);
}

static int pkvm_handle_exit(struct kvm_vcpu *vcpu, fastpath_t exit_fastpath)
{
	int ret = __pkvm_handle_exit(vcpu, exit_fastpath);

	/*
	 * Exit to user space when bus lock detected to inform that there is
	 * a bus lock in guest.
	 */
	if (to_vmx(vcpu)->exit_reason.bus_lock_detected) {
		if (ret > 0)
			vcpu->run->exit_reason = KVM_EXIT_X86_BUS_LOCK;

		vcpu->run->flags |= KVM_RUN_X86_BUS_LOCK;
		return 0;
	}
	return ret;
}

static int pkvm_skip_emulated_instruction(struct kvm_vcpu *vcpu)
{
	if (vcpu->arch.guest_state_protected)
		return 1;

	if (!kvm_emulate_instruction(vcpu, EMULTYPE_SKIP))
		return 0;

	/* skipping an emulated instruction also counts */
	kvm_call_pkvm(set_interrupt_shadow, vcpu, 0);

	return 1;
}

static void pkvm_update_emulated_instruction(struct kvm_vcpu *vcpu) {}

static void pkvm_set_interrupt_shadow(struct kvm_vcpu *vcpu, int mask)
{
	if (vcpu->arch.guest_state_protected)
		return;

	kvm_call_pkvm(set_interrupt_shadow, vcpu, mask);
}

static u32 pkvm_get_interrupt_shadow(struct kvm_vcpu *vcpu)
{
	if (vcpu->arch.guest_state_protected)
		return 0;

	return kvm_call_pkvm(get_interrupt_shadow, vcpu);
}

static void pkvm_inject_irq(struct kvm_vcpu *vcpu, bool reinjected)
{
	trace_kvm_inj_virq(vcpu->arch.interrupt.nr,
			   vcpu->arch.interrupt.soft, reinjected);

	++vcpu->stat.irq_injections;

	kvm_call_pkvm(inject_irq, vcpu);
}

static void pkvm_inject_nmi(struct kvm_vcpu *vcpu)
{
	++vcpu->stat.nmi_injections;

	kvm_call_pkvm(inject_nmi, vcpu);
}

static void pkvm_inject_exception(struct kvm_vcpu *vcpu)
{
	KVM_BUG_ON(pkvm_is_protected_vcpu(vcpu), vcpu->kvm);

	kvm_call_pkvm(inject_exception, vcpu);
}

static void pkvm_cancel_injection(struct kvm_vcpu *vcpu)
{
	vcpu->arch.nmi_injected = false;
	kvm_clear_exception_queue(vcpu);
	kvm_clear_interrupt_queue(vcpu);

	kvm_call_pkvm(cancel_injection, vcpu);

	if (vcpu->arch.nmi_injected ||
	    vcpu->arch.interrupt.injected ||
	    vcpu->arch.exception.injected)
		kvm_make_request(KVM_REQ_EVENT, vcpu);
}

static int pkvm_interrupt_allowed(struct kvm_vcpu *vcpu, bool for_injection)
{
	return kvm_call_pkvm(interrupt_allowed, vcpu, for_injection);
}

static int pkvm_nmi_allowed(struct kvm_vcpu *vcpu, bool for_injection)
{
	return kvm_call_pkvm(nmi_allowed, vcpu, for_injection);
}

static bool pkvm_get_nmi_mask(struct kvm_vcpu *vcpu)
{
	return kvm_call_pkvm(get_nmi_mask, vcpu);
}

static void pkvm_set_nmi_mask(struct kvm_vcpu *vcpu, bool masked)
{
	if (vcpu->arch.guest_state_protected)
		return;

	kvm_call_pkvm(set_nmi_mask, vcpu, masked);
}

static void pkvm_enable_nmi_window(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(enable_nmi_window, vcpu);
}

static void pkvm_enable_irq_window(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(enable_irq_window, vcpu);
}

static void pkvm_update_cr8_intercept(struct kvm_vcpu *vcpu, int tpr, int irr)
{
	kvm_call_pkvm(update_cr8_intercept, vcpu, tpr, irr);
}

static void pkvm_set_virtual_apic_mode(struct kvm_vcpu *vcpu)
{
	if (!lapic_in_kernel(vcpu))
		return;

	kvm_call_pkvm(set_virtual_apic_mode, vcpu, vcpu->arch.apic_base);
}

static void pkvm_set_apic_access_page_addr(struct kvm_vcpu *vcpu)
{
	/* No virtual apic access support in the pkvm hypervisor */
}

static void pkvm_refresh_apicv_exec_ctrl(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(refresh_apicv_exec_ctrl, vcpu);
}

static void pkvm_load_eoi_exitmap(struct kvm_vcpu *vcpu, u64 *eoi_exit_bitmap)
{
	u64 *exitmap;

	if (!kvm_vcpu_apicv_active(vcpu))
		return;

	exitmap = get_this_pv_param(eoi_exit_bitmap[0]);

	exitmap[0] = eoi_exit_bitmap[0];
	exitmap[1] = eoi_exit_bitmap[1];
	exitmap[2] = eoi_exit_bitmap[2];
	exitmap[3] = eoi_exit_bitmap[3];

	kvm_call_pkvm(load_eoi_exitmap, vcpu, exitmap);

	put_this_pv_param(exitmap);
}

static void pkvm_hwapic_irr_update(struct kvm_vcpu *vcpu, int max_irr)
{
	kvm_call_pkvm(hwapic_irr_update, vcpu, max_irr);
}

static void pkvm_hwapic_isr_update(struct kvm_vcpu *vcpu, int max_isr)
{
	kvm_call_pkvm(hwapic_isr_update, vcpu, max_isr);
}

static int pkvm_sync_pir_to_irr(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int max_irr;
	bool got_posted_interrupt;

	if (KVM_BUG_ON(!enable_apicv, vcpu->kvm))
		return -EIO;

	if (pi_test_on(&vmx->pi_desc)) {
		pi_clear_on(&vmx->pi_desc);
		/*
		 * IOMMU can write to PID.ON, so the barrier matters even on UP.
		 * But on x86 this is just a compiler barrier anyway.
		 */
		smp_mb__after_atomic();
		got_posted_interrupt =
			kvm_apic_update_irr(vcpu, vmx->pi_desc.pir, &max_irr);
	} else {
		max_irr = kvm_lapic_find_highest_irr(vcpu);
		got_posted_interrupt = false;
	}

	/*
	 * Newly recognized interrupts are injected via either virtual interrupt
	 * delivery (RVI) or KVM_REQ_EVENT.  Virtual interrupt delivery is
	 * disabled in two cases:
	 *
	 * 1) If L2 is running and the vCPU has a new pending interrupt.  If L1
	 * wants to exit on interrupts, KVM_REQ_EVENT is needed to synthesize a
	 * VM-Exit to L1.  If L1 doesn't want to exit, the interrupt is injected
	 * into L2, but KVM doesn't use virtual interrupt delivery to inject
	 * interrupts into L2, and so KVM_REQ_EVENT is again needed.
	 *
	 * 2) If APICv is disabled for this vCPU, assigned devices may still
	 * attempt to post interrupts.  The posted interrupt vector will cause
	 * a VM-Exit and the subsequent entry will call sync_pir_to_irr.
	 */
	if (!is_guest_mode(vcpu) && kvm_vcpu_apicv_active(vcpu) && max_irr != -1)
		kvm_call_pkvm(hwapic_irr_update, vcpu, max_irr);
	else if (got_posted_interrupt)
		kvm_make_request(KVM_REQ_EVENT, vcpu);

	return max_irr;
}

static void pkvm_get_exit_info(struct kvm_vcpu *vcpu, u32 *reason, u64 *info1,
			       u64 *info2, u32 *intr_info, u32 *error_code)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	*reason = vmx->exit_reason.full;
	*info1 = vmx_get_exit_qual(vcpu);
	if (!(vmx->exit_reason.failed_vmentry)) {
		*info2 = vmx->idt_vectoring_info;
		*intr_info = vmx->exit_intr_info;
		if (is_exception_with_error_code(*intr_info))
			*error_code = vmx->error_code;
		else
			*error_code = 0;
	} else {
		*info2 = 0;
		*intr_info = 0;
		*error_code = 0;
	}
}

static void pkvm_vcpu_after_set_cpuid(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *e2 = vcpu->arch.cpuid_entries;
	int nent = vcpu->arch.cpuid_nent;
	unsigned long unused_pa;
	void *entries;
	size_t size;

	if (vcpu->arch.guest_state_protected || !e2 || !nent)
		return;

	size = sizeof(struct kvm_cpuid_entry2) * nent;
	entries = alloc_pages_exact(size, GFP_KERNEL_ACCOUNT);
	if (!entries) {
		kvm_err("Failed to allocate cpuid pages for pkvm vcpu\n");
		return;
	}

	memcpy(entries, (void *)e2, size);

	unused_pa = kvm_call_pkvm(vcpu_after_set_cpuid, vcpu, __pa(entries));
	if (VALID_PAGE(unused_pa)) {
		entries = __va(unused_pa);
		free_pages_exact(entries, size);
	}
}

static u64 pkvm_get_l2_tsc_offset(struct kvm_vcpu *vcpu)
{
	return 0;
}

static u64 pkvm_get_l2_tsc_multiplier(struct kvm_vcpu *vcpu)
{
	return kvm_caps.default_tsc_scaling_ratio;
}

static void pkvm_write_tsc_offset(struct kvm_vcpu *vcpu)
{
	/*
	 * TODO: Not to write tsc_offset if the PV interface can be secure
	 * enforced.
	 */
	kvm_call_pkvm(write_tsc_offset, vcpu, vcpu->arch.tsc_offset);
}

static void pkvm_write_tsc_multiplier(struct kvm_vcpu *vcpu)
{
	/*
	 * TODO: Not to write tsc_multiplier if the PV interface can be secure
	 * enforced.
	 */
	kvm_call_pkvm(write_tsc_multiplier, vcpu, vcpu->arch.tsc_scaling_ratio);
}

static void pkvm_load_mmu_pgd(struct kvm_vcpu *vcpu, hpa_t root_hpa, int root_level)
{
	kvm_call_pkvm(load_mmu_pgd, vcpu, root_hpa, root_level);
}

static int pkvm_check_intercept(struct kvm_vcpu *vcpu,
				struct x86_instruction_info *info,
				enum x86_intercept_stage stage,
				struct x86_exception *exception)
{
	return X86EMUL_UNHANDLEABLE;
}

static void pkvm_setup_mce(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(setup_mce, vcpu, vcpu->arch.mcg_cap);
}

#ifdef CONFIG_KVM_SMM
static int pkvm_smi_allowed(struct kvm_vcpu *vcpu, bool for_injection)
{
	return false;
}

static int pkvm_enter_smm(struct kvm_vcpu *vcpu, union kvm_smram *smram)
{
	return -EINVAL;
}

static int pkvm_leave_smm(struct kvm_vcpu *vcpu, const union kvm_smram *smram)
{
	return -EINVAL;
}

static void pkvm_enable_smi_window(struct kvm_vcpu *vcpu) {}
#endif

static bool pkvm_apic_init_signal_blocked(struct kvm_vcpu *vcpu)
{
	return false;
}

static void pkvm_migrate_timers(struct kvm_vcpu *vcpu) {}

static int pkvm_check_emulate_instruction(struct kvm_vcpu *vcpu, int emul_type,
					  void *insn, int insn_len)
{
	/*
	 * This can only be triggered when the host is emulating a MMIO
	 * instruction. For the pVM, this shouldn't happen if the pVM is
	 * enlighted to use hypercall to access MMIO. Or the pVM still expects
	 * receiving #VE, then returns X86EMUL_RETRY_INSTR to let the pVM to retry
	 * after clearing the suppress #VE bit in the shadow EPT.
	 */
	if (pkvm_is_protected_vcpu(vcpu))
		return X86EMUL_RETRY_INSTR;

	/* For npVM, the instruction can be emulated */
	return X86EMUL_CONTINUE;
}

static void pkvm_msr_filter_changed(struct kvm_vcpu *vcpu) {}

static int pkvm_complete_emulated_msr(struct kvm_vcpu *vcpu, int err)
{
	if (err)
		return kvm_call_pkvm(complete_emulated_msr, vcpu, err);

	return pkvm_is_protected_vcpu(vcpu) ? 1 : kvm_skip_emulated_instruction(vcpu);
}

#define VMX_REQUIRED_APICV_INHIBITS				\
	(BIT(APICV_INHIBIT_REASON_DISABLED) |			\
	 BIT(APICV_INHIBIT_REASON_ABSENT) |			\
	 BIT(APICV_INHIBIT_REASON_HYPERV) |			\
	 BIT(APICV_INHIBIT_REASON_BLOCKIRQ) |			\
	 BIT(APICV_INHIBIT_REASON_PHYSICAL_ID_ALIASED) |	\
	 BIT(APICV_INHIBIT_REASON_APIC_ID_MODIFIED) |		\
	 BIT(APICV_INHIBIT_REASON_APIC_BASE_MODIFIED))

static void pkvm_leave_nested(struct kvm_vcpu *vcpu) {}
static bool pkvm_nested_is_exception_vmexit(struct kvm_vcpu *vcpu, u8 vector,
					    u32 error_code)
{
	return false;
}
static int pkvm_check_nested_events(struct kvm_vcpu *vcpu) { return 0; }
static bool pkvm_has_nested_events(struct kvm_vcpu *vcpu, bool for_injection) { return false; }
static void pkvm_nested_triple_fault(struct kvm_vcpu *vcpu) {}
static int pkvm_get_nested_state(struct kvm_vcpu *vcpu,
				 struct kvm_nested_state __user *user_kvm_nested_state,
				 u32 user_data_size)
{
	return -EINVAL;
}
static int pkvm_set_nested_state(struct kvm_vcpu *vcpu,
				 struct kvm_nested_state __user *user_kvm_nested_state,
				 struct kvm_nested_state *kvm_state)
{
	return -EINVAL;
}
static bool pkvm_get_nested_state_pages(struct kvm_vcpu *vcpu) { return true; }
static int pkvm_nested_write_pml_buffer(struct kvm_vcpu *vcpu, gpa_t gpa) { return 0; }

static struct kvm_x86_nested_ops pkvm_nested_ops = {
	.leave_nested = pkvm_leave_nested,
	.is_exception_vmexit = pkvm_nested_is_exception_vmexit,
	.check_events = pkvm_check_nested_events,
	.has_events = pkvm_has_nested_events,
	.triple_fault = pkvm_nested_triple_fault,
	.get_state = pkvm_get_nested_state,
	.set_state = pkvm_set_nested_state,
	.get_nested_state_pages = pkvm_get_nested_state_pages,
	.write_log_dirty = pkvm_nested_write_pml_buffer,
};

struct kvm_x86_ops pkvm_host_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.check_processor_compatibility = pkvm_check_processor_compat,

	.hardware_unsetup = vmx_hardware_unsetup,

	.enable_virtualization_cpu = pkvm_enable_virtualization_cpu,
	.disable_virtualization_cpu = pkvm_disable_virtualization_cpu,
	.emergency_disable_virtualization_cpu = pkvm_emergency_disable_virtualization_cpu,

	.has_emulated_msr = pkvm_has_emulated_msr,

	.vm_size = sizeof(struct kvm_vmx),
	.vm_init = pkvm_vm_init,
	.vm_destroy = pkvm_vm_destroy,

	.vcpu_precreate = vmx_vcpu_precreate,
	.vcpu_create = pkvm_vcpu_create,
	.vcpu_free = pkvm_vcpu_free,
	.vcpu_reset = pkvm_vcpu_reset,

	.prepare_switch_to_guest = pkvm_prepare_switch_to_guest,
	.vcpu_load = pkvm_vcpu_load,
	.vcpu_put = pkvm_vcpu_put,

	.update_exception_bitmap = pkvm_update_exception_bitmap,
	.get_feature_msr = pkvm_get_feature_msr,
	.get_msr = pkvm_get_msr,
	.set_msr = pkvm_set_msr,
	.get_segment_base = pkvm_get_segment_base,
	.get_segment = pkvm_get_segment,
	.set_segment = pkvm_set_segment,
	.get_cpl = pkvm_get_cpl,
	.get_cs_db_l_bits = pkvm_get_cs_db_l_bits,
	.is_valid_cr0 = pkvm_is_valid_cr0,
	.set_cr0 = pkvm_set_cr0,
	.post_set_cr3 = pkvm_post_set_cr3,
	.is_valid_cr4 = pkvm_is_valid_cr4,
	.set_cr4 = pkvm_set_cr4,
	.set_efer = pkvm_set_efer,
	.get_idt = pkvm_get_idt,
	.set_idt = pkvm_set_idt,
	.get_gdt = pkvm_get_gdt,
	.set_gdt = pkvm_set_gdt,
	.set_dr7 = pkvm_set_dr7,
	.sync_dirty_debug_regs = pkvm_sync_dirty_debug_regs,
	.cache_reg = pkvm_cache_reg,
	.get_rflags = pkvm_get_rflags,
	.set_rflags = pkvm_set_rflags,
	.get_if_flag = pkvm_get_if_flag,

	.flush_tlb_all = pkvm_flush_tlb_all,
	.flush_tlb_current = pkvm_flush_tlb_current,
	.flush_tlb_gva = pkvm_flush_tlb_gva,
	.flush_tlb_guest = pkvm_flush_tlb_guest,

	.vcpu_pre_run = vmx_vcpu_pre_run,
	.vcpu_run = pkvm_vcpu_run,
	.handle_exit = pkvm_handle_exit,
	.skip_emulated_instruction = pkvm_skip_emulated_instruction,
	.update_emulated_instruction = pkvm_update_emulated_instruction,
	.set_interrupt_shadow = pkvm_set_interrupt_shadow,
	.get_interrupt_shadow = pkvm_get_interrupt_shadow,
	.patch_hypercall = vmx_patch_hypercall,
	.inject_irq = pkvm_inject_irq,
	.inject_nmi = pkvm_inject_nmi,
	.inject_exception = pkvm_inject_exception,
	.cancel_injection = pkvm_cancel_injection,
	.interrupt_allowed = pkvm_interrupt_allowed,
	.nmi_allowed = pkvm_nmi_allowed,
	.get_nmi_mask = pkvm_get_nmi_mask,
	.set_nmi_mask = pkvm_set_nmi_mask,
	.enable_nmi_window = pkvm_enable_nmi_window,
	.enable_irq_window = pkvm_enable_irq_window,
	.update_cr8_intercept = pkvm_update_cr8_intercept,

	.x2apic_icr_is_split = false,
	.set_virtual_apic_mode = pkvm_set_virtual_apic_mode,
	.set_apic_access_page_addr = pkvm_set_apic_access_page_addr,
	.refresh_apicv_exec_ctrl = pkvm_refresh_apicv_exec_ctrl,
	.load_eoi_exitmap = pkvm_load_eoi_exitmap,
	.apicv_pre_state_restore = vmx_apicv_pre_state_restore,
	.required_apicv_inhibits = VMX_REQUIRED_APICV_INHIBITS,
	.hwapic_irr_update = pkvm_hwapic_irr_update,
	.hwapic_isr_update = pkvm_hwapic_isr_update,
	.sync_pir_to_irr = pkvm_sync_pir_to_irr,
	.deliver_interrupt = vmx_deliver_interrupt,
	.dy_apicv_has_pending_interrupt = pi_has_pending_interrupt,

	.set_tss_addr = vmx_set_tss_addr,
	.set_identity_map_addr = vmx_set_identity_map_addr,
	.get_mt_mask = vmx_get_mt_mask,

	.get_exit_info = pkvm_get_exit_info,

	.vcpu_after_set_cpuid = pkvm_vcpu_after_set_cpuid,

	.has_wbinvd_exit = pkvm_has_vmx_wbinvd_exit,

	.get_l2_tsc_offset = pkvm_get_l2_tsc_offset,
	.get_l2_tsc_multiplier = pkvm_get_l2_tsc_multiplier,
	.write_tsc_offset = pkvm_write_tsc_offset,
	.write_tsc_multiplier = pkvm_write_tsc_multiplier,

	.load_mmu_pgd = pkvm_load_mmu_pgd,

	.check_intercept = pkvm_check_intercept,
	.handle_exit_irqoff = vmx_handle_exit_irqoff,

	.cpu_dirty_log_size = PML_ENTITY_NUM,
	.update_cpu_dirty_logging = vmx_update_cpu_dirty_logging,

	.nested_ops = &pkvm_nested_ops,

	.pi_update_irte = vmx_pi_update_irte,
	.pi_start_assignment = vmx_pi_start_assignment,

#ifdef CONFIG_X86_64
	.set_hv_timer = vmx_set_hv_timer,
	.cancel_hv_timer = vmx_cancel_hv_timer,
#endif

	.setup_mce = pkvm_setup_mce,

#ifdef CONFIG_KVM_SMM
	.smi_allowed = pkvm_smi_allowed,
	.enter_smm = pkvm_enter_smm,
	.leave_smm = pkvm_leave_smm,
	.enable_smi_window = pkvm_enable_smi_window,
#endif

	.check_emulate_instruction = pkvm_check_emulate_instruction,
	.apic_init_signal_blocked = pkvm_apic_init_signal_blocked,
	.migrate_timers = pkvm_migrate_timers,

	.msr_filter_changed = pkvm_msr_filter_changed,
	.complete_emulated_msr = pkvm_complete_emulated_msr,

	.vcpu_deliver_sipi_vector = kvm_vcpu_deliver_sipi_vector,

	.get_untagged_addr = vmx_get_untagged_addr,
};

static struct kvm_pmc *pkvm_intel_rdpmc_ecx_to_pmc(struct kvm_vcpu *vcpu,
						   unsigned int idx, u64 *mask)
{
	return NULL;
}
static struct kvm_pmc *pkvm_intel_msr_idx_to_pmc(struct kvm_vcpu *vcpu, u32 msr) { return NULL; }
static bool pkvm_intel_is_valid_msr(struct kvm_vcpu *vcpu, u32 msr) { return false; }
static int pkvm_intel_pmu_get_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info) { return 1; }
static int pkvm_intel_pmu_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info) { return 1; }
static void pkvm_intel_pmu_refresh(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_init(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_reset(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_deliver_pmi(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_cleanup(struct kvm_vcpu *vcpu) {}

static struct kvm_pmu_ops pkvm_intel_pmu_ops __initdata = {
	.rdpmc_ecx_to_pmc = pkvm_intel_rdpmc_ecx_to_pmc,
	.msr_idx_to_pmc = pkvm_intel_msr_idx_to_pmc,
	.is_valid_msr = pkvm_intel_is_valid_msr,
	.get_msr = pkvm_intel_pmu_get_msr,
	.set_msr = pkvm_intel_pmu_set_msr,
	.refresh = pkvm_intel_pmu_refresh,
	.init = pkvm_intel_pmu_init,
	.reset = pkvm_intel_pmu_reset,
	.deliver_pmi = pkvm_intel_pmu_deliver_pmi,
	.cleanup = pkvm_intel_pmu_cleanup,
	.EVENTSEL_EVENT = ARCH_PERFMON_EVENTSEL_EVENT,
	.MAX_NR_GP_COUNTERS = 0,
	.MIN_NR_GP_COUNTERS = 0,
};

struct kvm_x86_init_ops pkvm_host_init_ops __initdata = {
	.hardware_setup = vmx_hardware_setup,
	.handle_intel_pt_intr = NULL,

	.runtime_ops = &pkvm_host_x86_ops,
	.pmu_ops = &pkvm_intel_pmu_ops,
};
