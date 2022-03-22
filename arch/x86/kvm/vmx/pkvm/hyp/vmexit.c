// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <pkvm.h>
#include "vmexit.h"
#include "debug.h"

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

static void handle_read_msr(struct kvm_vcpu *vcpu)
{
	/* simply return 0 for non-supported MSRs */
	vcpu->arch.regs[VCPU_REGS_RAX] = 0;
	vcpu->arch.regs[VCPU_REGS_RDX] = 0;
}

static void handle_write_msr(struct kvm_vcpu *vcpu)
{
	/*No emulation for msr write now*/
}

static void handle_xsetbv(struct kvm_vcpu *vcpu)
{
	u32 eax = (u32)(vcpu->arch.regs[VCPU_REGS_RAX] & -1u);
	u32 edx = (u32)(vcpu->arch.regs[VCPU_REGS_RDX] & -1u);
	u32 ecx = (u32)(vcpu->arch.regs[VCPU_REGS_RCX] & -1u);

	asm volatile(".byte 0x0f,0x01,0xd1"
			: : "a" (eax), "d" (edx), "c" (ecx));
}

/* we take use of kvm_vcpu structure, but not used all the fields */
int pkvm_main(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	int launch = 1;

	do {
		bool skip_instruction = false;

		if (__pkvm_vmx_vcpu_run(vcpu->arch.regs, launch)) {
			pkvm_err("%s: CPU%d run_vcpu failed with error 0x%x\n",
				__func__, vcpu->cpu, vmcs_read32(VM_INSTRUCTION_ERROR));
			return -EINVAL;
		}

		vcpu->arch.cr2 = native_read_cr2();

		vmx->exit_reason.full = vmcs_read32(VM_EXIT_REASON);
		vmx->exit_qualification = vmcs_readl(EXIT_QUALIFICATION);

		switch (vmx->exit_reason.full) {
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
		default:
			pkvm_dbg("CPU%d: Unsupported vmexit reason 0x%x.\n", vcpu->cpu, vmx->exit_reason.full);
			skip_instruction = true;
			break;
		}

		/* now only need vmresume */
		launch = 0;

		if (skip_instruction)
			skip_emulated_instruction();

		native_write_cr2(vcpu->arch.cr2);
	} while (1);

	return 0;
}
