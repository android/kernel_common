// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <pkvm.h>

#include "pkvm_hyp.h"
#include "debug.h"

enum VMXResult {
	VMsucceed,
	VMfailValid,
	VMfailInvalid,
};

static void nested_vmx_result(enum VMXResult result, int error_number)
{
	u64 rflags = vmcs_readl(GUEST_RFLAGS);

	rflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_PF | X86_EFLAGS_AF |
			X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);

	if (result == VMfailValid) {
		rflags |= X86_EFLAGS_ZF;
		vmcs_write32(VM_INSTRUCTION_ERROR, error_number);
	} else if (result == VMfailInvalid) {
		rflags |= X86_EFLAGS_CF;
	} else {
		/* VMsucceed, do nothing */
	}

	if (result != VMsucceed)
		pkvm_err("VMX failed: %d/%d", result, error_number);

	vmcs_writel(GUEST_RFLAGS, rflags);
}

static int get_vmx_mem_address(struct kvm_vcpu *vcpu, unsigned long exit_qualification,
			u32 vmx_instruction_info, gva_t *ret)
{
	gva_t off;
	struct kvm_segment s;

	/*
	 * According to Vol. 3B, "Information for VM Exits Due to Instruction
	 * Execution", on an exit, vmx_instruction_info holds most of the
	 * addressing components of the operand. Only the displacement part
	 * is put in exit_qualification (see 3B, "Basic VM-Exit Information").
	 * For how an actual address is calculated from all these components,
	 * refer to Vol. 1, "Operand Addressing".
	 */
	int  scaling = vmx_instruction_info & 3;
	int  addr_size = (vmx_instruction_info >> 7) & 7;
	bool is_reg = vmx_instruction_info & (1u << 10);
	int  seg_reg = (vmx_instruction_info >> 15) & 7;
	int  index_reg = (vmx_instruction_info >> 18) & 0xf;
	bool index_is_valid = !(vmx_instruction_info & (1u << 22));
	int  base_reg       = (vmx_instruction_info >> 23) & 0xf;
	bool base_is_valid  = !(vmx_instruction_info & (1u << 27));

	if (is_reg) {
		/* TODO: inject #UD */
		return 1;
	}

	/* Addr = segment_base + offset */
	/* offset = base + [index * scale] + displacement */
	off = exit_qualification; /* holds the displacement */
	if (addr_size == 1)
		off = (gva_t)sign_extend64(off, 31);
	else if (addr_size == 0)
		off = (gva_t)sign_extend64(off, 15);
	if (base_is_valid)
		off += vcpu->arch.regs[base_reg];
	if (index_is_valid)
		off += vcpu->arch.regs[index_reg] << scaling;

	if (seg_reg == VCPU_SREG_FS)
		s.base = vmcs_readl(GUEST_FS_BASE);
	if (seg_reg == VCPU_SREG_GS)
		s.base = vmcs_readl(GUEST_GS_BASE);

	/* TODO: support more cpu mode beside long mode */
	/*
	 * The effective address, i.e. @off, of a memory operand is truncated
	 * based on the address size of the instruction.  Note that this is
	 * the *effective address*, i.e. the address prior to accounting for
	 * the segment's base.
	 */
	if (addr_size == 1) /* 32 bit */
		off &= 0xffffffff;
	else if (addr_size == 0) /* 16 bit */
		off &= 0xffff;

	/*
	 * The virtual/linear address is never truncated in 64-bit
	 * mode, e.g. a 32-bit address size can yield a 64-bit virtual
	 * address when using FS/GS with a non-zero base.
	 */
	if (seg_reg == VCPU_SREG_FS || seg_reg == VCPU_SREG_GS)
		*ret = s.base + off;
	else
		*ret = off;

	/* TODO: check addr is canonical, otherwise inject #GP/#SS */

	return 0;
}

static int nested_vmx_get_vmptr(struct kvm_vcpu *vcpu, gpa_t *vmpointer,
				int *ret)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	gva_t gva;
	struct x86_exception e;
	int r;

	if (get_vmx_mem_address(vcpu, vmx->exit_qualification,
			vmcs_read32(VMX_INSTRUCTION_INFO), &gva)) {
		*ret = 1;
		return -EINVAL;
	}

	r = read_gva(vcpu, gva, vmpointer, sizeof(*vmpointer), &e);
	if (r < 0) {
		/*TODO: handle memory failure exception */
		*ret = 1;
		return -EINVAL;
	}

	return 0;
}

static int validate_vmcs_revision_id(struct kvm_vcpu *vcpu, gpa_t vmpointer)
{
	struct vmcs_config *vmcs_config = &pkvm_hyp->vmcs_config;
	u32 rev_id;

	read_gpa(vcpu, vmpointer, &rev_id, sizeof(rev_id));

	return (rev_id == vmx_basic_vmcs_revision_id(vmcs_config->basic));
}

static bool check_vmx_permission(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	bool permit = true;

	/*TODO: check more env (cr, cpl) and inject #UD/#GP */
	if (!vmx->nested.vmxon)
		permit = false;

	return permit;
}

int handle_vmxon(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	gpa_t vmptr;
	int r;

	/*TODO: check env error(cr, efer, rflags, cpl) */
	if (vmx->nested.vmxon) {
		nested_vmx_result(VMfailValid, VMXERR_VMXON_IN_VMX_ROOT_OPERATION);
	} else {
		if (nested_vmx_get_vmptr(vcpu, &vmptr, &r)) {
			nested_vmx_result(VMfailInvalid, 0);
			return r;
		} else if (!validate_vmcs_revision_id(vcpu, vmptr)) {
			nested_vmx_result(VMfailInvalid, 0);
		} else {
			vmx->nested.vmxon_ptr = vmptr;
			vmx->nested.vmxon = true;

			nested_vmx_result(VMsucceed, 0);
		}
	}

	return 0;
}

int handle_vmxoff(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (check_vmx_permission(vcpu)) {
		vmx->nested.vmxon = false;
		vmx->nested.vmxon_ptr = INVALID_GPA;

		nested_vmx_result(VMsucceed, 0);
	}

	return 0;
}
