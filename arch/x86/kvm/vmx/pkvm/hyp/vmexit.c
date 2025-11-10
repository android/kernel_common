// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/memblock.h>
#include <asm/kvm_pkvm.h>
#include <vmx/x86_ops.h>
#include <pkvm/pkvm.h>
#include <pkvm.h>
#include "trace.h"
#include "vmexit.h"
#include "ept.h"
#include "pkvm_hyp.h"
#include "vmx.h"
#include "vmsr.h"
#include "iommu.h"
#include "lapic.h"
#include "debug.h"
#include "init_finalise.h"

#define CR4	4

#define MOV_TO_CR		0

static void skip_emulated_instruction(void)
{
	unsigned long rip;

	rip = vmcs_readl(GUEST_RIP);
	rip += vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	vmcs_writel(GUEST_RIP, rip);
}

static void handle_cpuid(struct kvm_vcpu *vcpu)
{
	u32 eax, ebx, ecx, edx;

	eax = vcpu->arch.regs[VCPU_REGS_RAX];
	ecx = vcpu->arch.regs[VCPU_REGS_RCX];
	native_cpuid(&eax, &ebx, &ecx, &edx);
	vcpu->arch.regs[VCPU_REGS_RAX] = eax;
	vcpu->arch.regs[VCPU_REGS_RBX] = ebx;
	vcpu->arch.regs[VCPU_REGS_RCX] = ecx;
	vcpu->arch.regs[VCPU_REGS_RDX] = edx;
}

static void handle_cr(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long exit_qual, val;
	int cr;
	int type;
	int reg;

	exit_qual = vmx->exit_qualification;
	cr = exit_qual & 15;
	type = (exit_qual >> 4)	& 3;
	reg = (exit_qual >> 8) & 15;

	switch (type) {
	case MOV_TO_CR:
		switch (cr) {
		case CR4:
			/*
			 * VMXE bit is owned by host, others are owned by guest
			 * So only when guest is trying to modify VMXE bit it
			 * can cause vmexit and get here.
			 */
			val = vcpu->arch.regs[reg];
			vmcs_writel(CR4_READ_SHADOW, val);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static unsigned long handle_vmcall(struct kvm_vcpu *vcpu)
{
	u64 nr, a0, a1, a2, a3, a4;
	unsigned long ret = 0;

	nr = vcpu->arch.regs[VCPU_REGS_RAX];
	a0 = vcpu->arch.regs[VCPU_REGS_RBX];
	a1 = vcpu->arch.regs[VCPU_REGS_RCX];
	a2 = vcpu->arch.regs[VCPU_REGS_RDX];
	a3 = vcpu->arch.regs[VCPU_REGS_RSI];
	a4 = vcpu->arch.regs[VCPU_REGS_RDI];

	switch (nr) {
	case __pkvm__init_finalize:
		ret = __pkvm_init_finalise(vcpu, (struct pkvm_section *)a0, a1);
		break;
	case __pkvm__commit_finalize:
		ret = pkvm_commit_finalise(a0);
		break;
	case __pkvm__reprivilege_cpu:
		ret = pkvm_reprivilege_vcpu(vcpu);
		/* Reach here only if reprivilege fails. */
		break;
	case __pkvm__set_vmexit_trace:
		pkvm_handle_set_vmexit_trace(a0);
		break;
	case __pkvm__dump_vmexit_trace:
		pkvm_handle_dump_vmexit_trace(a0, a1, a2);
		break;
	case __pkvm__iommu_mmio_access:
		ret = pkvm_access_iommu(a0, a1, a2, a3);
		break;
	case __pkvm__add_ptdev:
		ret = pkvm_add_ptdev(a0, a1, a2);
		break;
	default:
		ret = handle_kvm_call(nr, a0, a1, a2, a3, a4);
	}

	return ret;
}

static void handle_xsetbv(struct kvm_vcpu *vcpu)
{
	u32 eax = (u32)(vcpu->arch.regs[VCPU_REGS_RAX] & -1u);
	u32 edx = (u32)(vcpu->arch.regs[VCPU_REGS_RDX] & -1u);
	u32 ecx = (u32)(vcpu->arch.regs[VCPU_REGS_RCX] & -1u);

	asm volatile(".byte 0x0f,0x01,0xd1"
			: : "a" (eax), "d" (edx), "c" (ecx));
}

static void handle_irq_window(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	u32 cpu_based_exec_ctrl = exec_controls_get(vmx);

	exec_controls_set(vmx, cpu_based_exec_ctrl & ~CPU_BASED_INTR_WINDOW_EXITING);
	pkvm_dbg("%s: CPU%d clear irq_window_exiting\n", __func__, vcpu->cpu);

	kvm_make_request(KVM_REQ_EVENT, vcpu);
}

static void handle_preemption_timer(struct kvm_vcpu *vcpu)
{
	pin_controls_clearbit(to_vmx(vcpu), PIN_BASED_VMX_PREEMPTION_TIMER);
}

static void inject_pending_nmi(struct kvm_vcpu *vcpu)
{
	if (!vcpu->arch.nmi_pending)
		return;

	/*
	 * Check for the NMI blocking and inject the NMI only when it is not
	 * blocked.
	 * The vmx code vmx_nmi_blocked() and vmx_inject_nmi() are not used at
	 * here as their implementation is related with the global parameter
	 * enable_vnmi which can determine how the guest VMs handle the NMI. The
	 * host VM has physical NMI passthrough which is not exactly fitting to
	 * the usage of enable_vnmi.
	 */
	if (!(vmcs_read32(GUEST_INTERRUPTIBILITY_INFO) &
	      (GUEST_INTR_STATE_MOV_SS | GUEST_INTR_STATE_STI |
	       GUEST_INTR_STATE_NMI))) {
		--vcpu->arch.nmi_pending;
		vmcs_write32(VM_ENTRY_INTR_INFO_FIELD,
			     INTR_TYPE_NMI_INTR | INTR_INFO_VALID_MASK | NMI_VECTOR);

		/* clear the HLT state */
		if (vmcs_read32(GUEST_ACTIVITY_STATE) == GUEST_ACTIVITY_HLT)
			vmcs_write32(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
	}

	/*
	 * If there are more pending NMI, open the irq window to inject the
	 * pending ones when the NMI is unblocked. Using irq window rather than
	 * the NMI window since this is for the physical NMI, while NMI window
	 * is for virtual-NMI when virtual-NMI execution control is enabled,
	 * which is not used for the host VM.
	 */
	if (vcpu->arch.nmi_pending)
		vmx_enable_irq_window(vcpu);
}

static void handle_pending_events(struct kvm_vcpu *vcpu, bool *req_immediate_exit)
{
	if (kvm_check_request(KVM_REQ_NMI, vcpu)) {
		vcpu->arch.nmi_pending += atomic_xchg(&vcpu->arch.nmi_queued, 0);
		kvm_make_request(KVM_REQ_EVENT, vcpu);
	}

	if (kvm_check_request(KVM_REQ_EVENT, vcpu)) {
		if (vcpu->arch.exception.pending) {
			vmx_inject_exception(vcpu);
			vcpu->arch.exception.pending = false;
			vcpu->arch.exception.injected = true;
		}

		if (vcpu->arch.nmi_pending) {
			/*
			 * Inject pending NMI if no exception is already injected.
			 * Otherwise request an immediate exit to inject NMI in the
			 * next vmexit.
			 */
			if (!vcpu->arch.exception.injected)
				inject_pending_nmi(vcpu);
			else
				*req_immediate_exit = true;
		}
	}

	if (kvm_check_request(PKVM_REQ_TLB_FLUSH_HOST_EPT, vcpu))
		pkvm_flush_host_ept();
}

static inline void set_vcpu_mode(struct kvm_vcpu *vcpu, int mode)
{
	vcpu->mode = mode;
	/*
	 * Make sure vcpu->mode is set before checking/handling the pending
	 * requests. Pairs with kvm_vcpu_exiting_guest_mode().
	 */
	smp_wmb();
}

/*
 * we take use of kvm_vcpu structure, but not used all the fields.
 */
void pkvm_vmexit_main(struct vcpu_vmx *vmx)
{
	struct kvm_vcpu *vcpu = &vmx->vcpu;
	bool req_immediate_exit = false;
	bool skip_instruction = false;

	vcpu->arch.cr2 = native_read_cr2();

	trace_vmexit_start(vcpu);

	set_vcpu_mode(vcpu, OUTSIDE_GUEST_MODE);

	vcpu->arch.cr3 = vmcs_readl(GUEST_CR3);
	vcpu->arch.regs[VCPU_REGS_RSP] = vmcs_readl(GUEST_RSP);

	vcpu->arch.exception.injected = false;

	vmx->exit_reason.full = vmcs_read32(VM_EXIT_REASON);
	vmx->exit_qualification = vmcs_readl(EXIT_QUALIFICATION);

	switch (vmx->exit_reason.full) {
	case EXIT_REASON_INIT_SIGNAL:
		/*
		 * INIT is used as kick when making a request.
		 * So just break the vmexits and go to pending
		 * events handling.
		 */
		break;
	case EXIT_REASON_CPUID:
		handle_cpuid(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_CR_ACCESS:
		pkvm_dbg("CPU%d vmexit_reason: CR_ACCESS.\n", vcpu->cpu);
		handle_cr(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_MSR_READ:
		pkvm_dbg("CPU%d vmexit_reason: MSR_READ 0x%lx\n",
				vcpu->cpu, vcpu->arch.regs[VCPU_REGS_RCX]);
		handle_read_msr(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_MSR_WRITE:
		pkvm_dbg("CPU%d vmexit_reason: MSR_WRITE 0x%lx\n",
				vcpu->cpu, vcpu->arch.regs[VCPU_REGS_RCX]);
		handle_write_msr(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_XSETBV:
		handle_xsetbv(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_VMCALL:
		vcpu->arch.regs[VCPU_REGS_RAX] = handle_vmcall(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_EPT_VIOLATION:
		if (handle_host_ept_violation(vcpu)) {
			pkvm_err("pkvm: handle host ept violation failed\n");
			kvm_inject_gp(vcpu, 0);
		}
		break;
	case EXIT_REASON_INTERRUPT_WINDOW:
		handle_irq_window(vcpu);
		break;
	case EXIT_REASON_PREEMPTION_TIMER:
		handle_preemption_timer(vcpu);
		break;
	default:
		pkvm_dbg("CPU%d: Unsupported vmexit reason 0x%x.\n",
			 vcpu->cpu, vmx->exit_reason.full);
		skip_instruction = true;
		break;
	}

	if (skip_instruction)
		skip_emulated_instruction();

handle_events:
	handle_pending_events(vcpu, &req_immediate_exit);

	set_vcpu_mode(vcpu, IN_GUEST_MODE);

	if (req_immediate_exit) {
		kvm_make_request(KVM_REQ_EVENT, vcpu);
		request_host_immediate_exit(vmx);
	} else if (vcpu->mode == EXITING_GUEST_MODE || kvm_request_pending(vcpu)) {
		goto handle_events;
	}

	native_write_cr2(vcpu->arch.cr2);
	trace_vmexit_end(vcpu, vmx->exit_reason.basic);

	if (static_branch_unlikely(&vmx_l1d_should_flush))
		vmx_l1d_flush(vcpu);
	else if (static_branch_unlikely(&mmio_stale_data_clear))
		x86_clear_cpu_buffers();
}
