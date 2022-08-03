// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/memblock.h>
#include <asm/kvm_pkvm.h>
#include <pkvm.h>
#include "trace.h"
#include "vmexit.h"
#include "ept.h"
#include "pkvm_hyp.h"
#include "vmsr.h"
#include "nested.h"
#include "lapic.h"
#include "debug.h"

#define CR4	4

#define MOV_TO_CR		0

extern int __pkvm_init_finalise(struct kvm_vcpu *vcpu,
		phys_addr_t phys, unsigned long size);

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
	u64 nr, a0, a1, a2, a3;
	unsigned long ret = 0;

	nr = vcpu->arch.regs[VCPU_REGS_RAX];
	a0 = vcpu->arch.regs[VCPU_REGS_RBX];
	a1 = vcpu->arch.regs[VCPU_REGS_RCX];
	a2 = vcpu->arch.regs[VCPU_REGS_RDX];
	a3 = vcpu->arch.regs[VCPU_REGS_RSI];

	switch (nr) {
	case PKVM_HC_SET_VMEXIT_TRACE:
		pkvm_handle_set_vmexit_trace(vcpu, a0);
		break;
	case PKVM_HC_DUMP_VMEXIT_TRACE:
		pkvm_handle_dump_vmexit_trace(a0, a1);
		break;
	case PKVM_HC_INIT_FINALISE:
		__pkvm_init_finalise(vcpu, a0, a1);
		break;
	case PKVM_HC_INIT_SHADOW_VM:
		ret = __pkvm_init_shadow_vm(vcpu, a0, a1, a2);
		break;
	case PKVM_HC_INIT_SHADOW_VCPU:
		ret = __pkvm_init_shadow_vcpu(vcpu, a0, a1, a2, a3);
		break;
	case PKVM_HC_TEARDOWN_SHADOW_VM:
		ret = __pkvm_teardown_shadow_vm(a0);
		break;
	case PKVM_HC_TEARDOWN_SHADOW_VCPU:
		ret = __pkvm_teardown_shadow_vcpu(a0);
		break;
	default:
		ret = -EINVAL;
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
}

static void handle_pending_events(struct kvm_vcpu *vcpu)
{
	struct pkvm_host_vcpu *pkvm_host_vcpu = to_pkvm_hvcpu(vcpu);

	if (!is_guest_mode(vcpu) && pkvm_host_vcpu->pending_nmi) {
		/* Inject if NMI is not blocked */
		vmcs_write32(VM_ENTRY_INTR_INFO_FIELD,
			     INTR_TYPE_NMI_INTR | INTR_INFO_VALID_MASK | NMI_VECTOR);
		pkvm_host_vcpu->pending_nmi = false;
	}

	if (kvm_check_request(PKVM_REQ_TLB_FLUSH_HOST_EPT, vcpu))
		pkvm_flush_host_ept();
	if (kvm_check_request(PKVM_REQ_TLB_FLUSH_SHADOW_EPT, vcpu))
		nested_flush_shadow_ept(vcpu);
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

/* we take use of kvm_vcpu structure, but not used all the fields */
int pkvm_main(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int launch = 1;

	vcpu->mode = IN_GUEST_MODE;

	do {
		bool skip_instruction = false, guest_exit = false;

		if (__pkvm_vmx_vcpu_run(vcpu->arch.regs, launch)) {
			pkvm_err("%s: CPU%d run_vcpu failed with error 0x%x\n",
				__func__, vcpu->cpu, vmcs_read32(VM_INSTRUCTION_ERROR));
			return -EINVAL;
		}

		vcpu->arch.cr2 = native_read_cr2();

		trace_vmexit_start(vcpu, is_guest_mode(vcpu) ? true : false);

		set_vcpu_mode(vcpu, OUTSIDE_GUEST_MODE);

		vcpu->arch.cr3 = vmcs_readl(GUEST_CR3);
		vcpu->arch.regs[VCPU_REGS_RSP] = vmcs_readl(GUEST_RSP);

		vmx->exit_reason.full = vmcs_read32(VM_EXIT_REASON);
		vmx->exit_qualification = vmcs_readl(EXIT_QUALIFICATION);

		if (is_guest_mode(vcpu)) {
			guest_exit = true;
			nested_vmexit(vcpu, &skip_instruction);
		} else {
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
			case EXIT_REASON_VMLAUNCH:
				handle_vmlaunch(vcpu);
				break;
			case EXIT_REASON_VMRESUME:
				handle_vmresume(vcpu);
				break;
			case EXIT_REASON_VMON:
				pkvm_dbg("CPU%d vmexit reason: VMXON.\n", vcpu->cpu);
				handle_vmxon(vcpu);
				skip_instruction = true;
				break;
			case EXIT_REASON_VMOFF:
				pkvm_dbg("CPU%d vmexit reason: VMXOFF.\n", vcpu->cpu);
				handle_vmxoff(vcpu);
				skip_instruction = true;
				break;
			case EXIT_REASON_VMPTRLD:
				pkvm_dbg("CPU%d vmexit reason: VMPTRLD.\n", vcpu->cpu);
				handle_vmptrld(vcpu);
				skip_instruction = true;
				break;
			case EXIT_REASON_VMCLEAR:
				pkvm_dbg("CPU%d vmexit reason: VMCLEAR.\n", vcpu->cpu);
				handle_vmclear(vcpu);
				skip_instruction = true;
				break;
			case EXIT_REASON_VMREAD:
				pkvm_dbg("CPU%d vmexit reason: WMREAD.\n", vcpu->cpu);
				handle_vmread(vcpu);
				skip_instruction = true;
				break;
			case EXIT_REASON_VMWRITE:
				pkvm_dbg("CPU%d vmexit reason: VMWRITE.\n", vcpu->cpu);
				handle_vmwrite(vcpu);
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
				if (handle_host_ept_violation(vmcs_read64(GUEST_PHYSICAL_ADDRESS)))
					skip_instruction = true;
				break;
			case EXIT_REASON_INTERRUPT_WINDOW:
				handle_irq_window(vcpu);
				break;
			case EXIT_REASON_INVEPT:
				handle_invept(vcpu);
				skip_instruction = true;
				break;
			case EXIT_REASON_INVVPID:
				ept_sync_global();
				skip_instruction = true;
				break;
			default:
				pkvm_dbg("CPU%d: Unsupported vmexit reason 0x%x.\n", vcpu->cpu, vmx->exit_reason.full);
				skip_instruction = true;
				break;
			}
		}

		if (skip_instruction)
			skip_emulated_instruction();
handle_events:
		handle_pending_events(vcpu);

		set_vcpu_mode(vcpu, IN_GUEST_MODE);

		if (vcpu->mode == EXITING_GUEST_MODE || kvm_request_pending(vcpu))
			goto handle_events;

		/*
		 * L2 VMExit -> L1 VMEntry and L1 VMExit -> L1 VMEnetry: vmresume.
		 * L2 VMExit -> L2 VMEntry: vmresume
		 * L1 VMExit -> L2 VMEntry: vmlaunch, as vmcs02 is clear every time
		 */
		launch = !is_guest_mode(vcpu) ? 0 : (guest_exit ? 0 : 1);

		native_write_cr2(vcpu->arch.cr2);
		trace_vmexit_end(vcpu, vmx->exit_reason.basic);
	} while (1);

	return 0;
}
