// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <pkvm.h>

#include "pkvm_hyp.h"
#include "nested.h"
#include "cpu.h"
#include "vmx.h"
#include "ept.h"
#include "debug.h"

/*
 * Not support shadow vmcs & vmfunc;
 * Not support descriptor-table exiting
 * as it requires guest memory access
 * to decode and emulate instructions
 * which is not supported for protected VM.
 */
#define NESTED_UNSUPPORTED_2NDEXEC 		\
	(SECONDARY_EXEC_SHADOW_VMCS | 		\
	 SECONDARY_EXEC_ENABLE_VMFUNC | 	\
	 SECONDARY_EXEC_DESC)

static const unsigned int vmx_msrs[] = {
	LIST_OF_VMX_MSRS
};

bool is_vmx_msr(unsigned long msr)
{
	bool found = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(vmx_msrs); i++) {
		if (msr == vmx_msrs[i]) {
			found = true;
			break;
		}
	}

	return found;
}

int read_vmx_msr(struct kvm_vcpu *vcpu, unsigned long msr, u64 *val)
{
	u32 low, high;
	int err = 0;

	pkvm_rdmsr(msr, low, high);

	switch (msr) {
	case MSR_IA32_VMX_PROCBASED_CTLS2:
		high &= ~NESTED_UNSUPPORTED_2NDEXEC;
		break;
	case MSR_IA32_VMX_MISC:
		/* not support PT, SMM */
		low &= ~(VMX_MISC_INTEL_PT | BIT(28));
		break;
	case MSR_IA32_VMX_VMFUNC:
		/* not support vmfunc */
		low = high = 0;
		break;
	default:
		err = -EACCES;
		break;
	}

	*val = (u64)high << 32 | (u64)low;

	return err;
}

/**
 * According to SDM Appendix B Field Encoding in VMCS, some fields only
 * exist on processor that support the 1-setting of the corresponding
 * fields in the control regs.
 */
static bool has_vmcs_field(u16 encoding)
{
	struct nested_vmx_msrs *msrs = &pkvm_hyp->vmcs_config.nested;

	switch (encoding) {
	case MSR_BITMAP:
		return msrs->procbased_ctls_high & CPU_BASED_USE_MSR_BITMAPS;
	case VIRTUAL_APIC_PAGE_ADDR:
	case VIRTUAL_APIC_PAGE_ADDR_HIGH:
	case TPR_THRESHOLD:
		return msrs->procbased_ctls_high & CPU_BASED_TPR_SHADOW;
	case SECONDARY_VM_EXEC_CONTROL:
		return msrs->procbased_ctls_high &
			CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;

	case VIRTUAL_PROCESSOR_ID:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_ENABLE_VPID;
	case XSS_EXIT_BITMAP:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_ENABLE_XSAVES;
	case PML_ADDRESS:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_ENABLE_PML;
	case VM_FUNCTION_CONTROL:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_ENABLE_VMFUNC;
	case EPT_POINTER:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_ENABLE_EPT;
	case EOI_EXIT_BITMAP0:
	case EOI_EXIT_BITMAP1:
	case EOI_EXIT_BITMAP2:
	case EOI_EXIT_BITMAP3:
		return msrs->secondary_ctls_high &
			SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY;
	case VMREAD_BITMAP:
	case VMWRITE_BITMAP:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_SHADOW_VMCS;
	case ENCLS_EXITING_BITMAP:
		return msrs->secondary_ctls_high &
			SECONDARY_EXEC_ENCLS_EXITING;
	case GUEST_INTR_STATUS:
		return msrs->secondary_ctls_high &
			SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY;
	case GUEST_PML_INDEX:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_ENABLE_PML;
	case APIC_ACCESS_ADDR:
	case APIC_ACCESS_ADDR_HIGH:
		return msrs->secondary_ctls_high &
			SECONDARY_EXEC_VIRTUALIZE_APIC_ACCESSES;
	case TSC_MULTIPLIER:
	case TSC_MULTIPLIER_HIGH:
		return msrs->secondary_ctls_high &
			SECONDARY_EXEC_TSC_SCALING;
	case GUEST_PHYSICAL_ADDRESS:
	case GUEST_PHYSICAL_ADDRESS_HIGH:
		return msrs->secondary_ctls_high &
			SECONDARY_EXEC_ENABLE_EPT;
	case GUEST_PDPTR0:
	case GUEST_PDPTR0_HIGH:
	case GUEST_PDPTR1:
	case GUEST_PDPTR1_HIGH:
	case GUEST_PDPTR2:
	case GUEST_PDPTR2_HIGH:
	case GUEST_PDPTR3:
	case GUEST_PDPTR3_HIGH:
		return msrs->secondary_ctls_high & SECONDARY_EXEC_ENABLE_EPT;
	case PLE_GAP:
	case PLE_WINDOW:
		return msrs->secondary_ctls_high &
			SECONDARY_EXEC_PAUSE_LOOP_EXITING;

	case VMX_PREEMPTION_TIMER_VALUE:
		return msrs->pinbased_ctls_high &
			PIN_BASED_VMX_PREEMPTION_TIMER;
	case POSTED_INTR_DESC_ADDR:
		return msrs->pinbased_ctls_high & PIN_BASED_POSTED_INTR;
	case POSTED_INTR_NV:
		return msrs->pinbased_ctls_high & PIN_BASED_POSTED_INTR;
	case GUEST_IA32_PAT:
	case GUEST_IA32_PAT_HIGH:
		return (msrs->entry_ctls_high & VM_ENTRY_LOAD_IA32_PAT) ||
			(msrs->exit_ctls_high & VM_EXIT_SAVE_IA32_PAT);
	case GUEST_IA32_EFER:
	case GUEST_IA32_EFER_HIGH:
		return (msrs->entry_ctls_high & VM_ENTRY_LOAD_IA32_EFER) ||
			(msrs->exit_ctls_high & VM_EXIT_SAVE_IA32_EFER);
	case GUEST_IA32_PERF_GLOBAL_CTRL:
	case GUEST_IA32_PERF_GLOBAL_CTRL_HIGH:
		return msrs->entry_ctls_high & VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL;
	case GUEST_BNDCFGS:
	case GUEST_BNDCFGS_HIGH:
		return (msrs->entry_ctls_high & VM_ENTRY_LOAD_BNDCFGS) ||
			(msrs->exit_ctls_high & VM_EXIT_CLEAR_BNDCFGS);
	case GUEST_IA32_RTIT_CTL:
	case GUEST_IA32_RTIT_CTL_HIGH:
		return (msrs->entry_ctls_high & VM_ENTRY_LOAD_IA32_RTIT_CTL) ||
			(msrs->exit_ctls_high & VM_EXIT_CLEAR_IA32_RTIT_CTL);
	case HOST_IA32_PAT:
	case HOST_IA32_PAT_HIGH:
		return msrs->exit_ctls_high & VM_EXIT_LOAD_IA32_PAT;
	case HOST_IA32_EFER:
	case HOST_IA32_EFER_HIGH:
		return msrs->exit_ctls_high & VM_EXIT_LOAD_IA32_EFER;
	case HOST_IA32_PERF_GLOBAL_CTRL:
	case HOST_IA32_PERF_GLOBAL_CTRL_HIGH:
		return msrs->exit_ctls_high & VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL;
	case EPTP_LIST_ADDRESS:
		return msrs->vmfunc_controls & VMX_VMFUNC_EPTP_SWITCHING;
	default:
		return true;
	}
}

enum VMXResult {
	VMsucceed,
	VMfailValid,
	VMfailInvalid,
};

struct shadow_vmcs_field {
	u16	encoding;
	u16	offset;
};

static u8 vmx_vmread_bitmap[PAGE_SIZE] __aligned(PAGE_SIZE);
static u8 vmx_vmwrite_bitmap[PAGE_SIZE] __aligned(PAGE_SIZE);

static struct shadow_vmcs_field shadow_read_only_fields[] = {
#define SHADOW_FIELD_RO(x, y) { x, offsetof(struct vmcs12, y) },
#include "pkvm_nested_vmcs_fields.h"
};
static int max_shadow_read_only_fields =
	ARRAY_SIZE(shadow_read_only_fields);
static struct shadow_vmcs_field shadow_read_write_fields[] = {
#define SHADOW_FIELD_RW(x, y) { x, offsetof(struct vmcs12, y) },
#include "pkvm_nested_vmcs_fields.h"
};
static int max_shadow_read_write_fields =
	ARRAY_SIZE(shadow_read_write_fields);
static struct shadow_vmcs_field emulated_fields[] = {
#define EMULATED_FIELD_RW(x, y) { x, offsetof(struct vmcs12, y) },
#include "pkvm_nested_vmcs_fields.h"
};
static int max_emulated_fields =
	ARRAY_SIZE(emulated_fields);

static void init_vmcs_shadow_fields(void)
{
	int i, j;

	memset(vmx_vmread_bitmap, 0xff, PAGE_SIZE);
	memset(vmx_vmwrite_bitmap, 0xff, PAGE_SIZE);

	for (i = j = 0; i < max_shadow_read_only_fields; i++) {
		struct shadow_vmcs_field entry = shadow_read_only_fields[i];
		u16 field = entry.encoding;

		if (!has_vmcs_field(field))
			continue;

		if (vmcs_field_width(field) == VMCS_FIELD_WIDTH_U64 &&
		    (i + 1 == max_shadow_read_only_fields ||
		     shadow_read_only_fields[i + 1].encoding != field + 1)) {
			pkvm_err("Missing field from shadow_read_only_field %x\n",
			       field + 1);
		}

		clear_bit(field, (unsigned long *)vmx_vmread_bitmap);
		if (field & 1)
			continue;
		shadow_read_only_fields[j++] = entry;
	}
	max_shadow_read_only_fields = j;

	for (i = j = 0; i < max_shadow_read_write_fields; i++) {
		struct shadow_vmcs_field entry = shadow_read_write_fields[i];
		u16 field = entry.encoding;

		if (!has_vmcs_field(field))
			continue;

		if (vmcs_field_width(field) == VMCS_FIELD_WIDTH_U64 &&
		    (i + 1 == max_shadow_read_write_fields ||
		     shadow_read_write_fields[i + 1].encoding != field + 1)) {
			pkvm_err("Missing field from shadow_read_write_field %x\n",
			       field + 1);
		}

		clear_bit(field, (unsigned long *)vmx_vmwrite_bitmap);
		clear_bit(field, (unsigned long *)vmx_vmread_bitmap);
		if (field & 1)
			continue;
		shadow_read_write_fields[j++] = entry;
	}
	max_shadow_read_write_fields = j;
}

static void init_emulated_vmcs_fields(void)
{
	int i, j;

	for (i = j = 0; i < max_emulated_fields; i++) {
		struct shadow_vmcs_field entry = emulated_fields[i];
		u16 field = entry.encoding;

		if (!has_vmcs_field(field))
			continue;

		emulated_fields[j++] = entry;
	}
	max_emulated_fields = j;
}

static bool is_host_fields(unsigned long field)
{
	return (((field) >> 10U) & 0x3U) == 3U;
}

static bool is_emulated_fields(unsigned long field_encoding)
{
	int i;

	for (i = 0; i < max_emulated_fields; i++) {
		if ((unsigned long)emulated_fields[i].encoding == field_encoding)
			return true;
	}

	return false;
}

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

static void clear_shadow_indicator(struct vmcs *vmcs)
{
	vmcs->hdr.shadow_vmcs = 0;
}

static void set_shadow_indicator(struct vmcs *vmcs)
{
	vmcs->hdr.shadow_vmcs = 1;
}

/* current vmcs is vmcs02 */
static void copy_shadow_fields_vmcs02_to_vmcs12(struct vcpu_vmx *vmx, struct vmcs12 *vmcs12)
{
	const struct shadow_vmcs_field *fields[] = {
		shadow_read_write_fields,
		shadow_read_only_fields
	};
	const int max_fields[] = {
		max_shadow_read_write_fields,
		max_shadow_read_only_fields
	};
	struct shadow_vmcs_field field;
	unsigned long val;
	int i, q;

	for (q = 0; q < ARRAY_SIZE(fields); q++) {
		for (i = 0; i < max_fields[q]; i++) {
			field = fields[q][i];
			val = __vmcs_readl(field.encoding);
			if (is_host_fields((field.encoding))) {
				pkvm_err("%s: field 0x%x is host field, please remove from shadowing!",
						__func__, field.encoding);
				continue;
			}
			vmcs12_write_any(vmcs12, field.encoding, field.offset, val);
		}
	}
}

/* current vmcs is vmcs02 */
static void copy_shadow_fields_vmcs12_to_vmcs02(struct vcpu_vmx *vmx, struct vmcs12 *vmcs12)
{
	const struct shadow_vmcs_field *fields[] = {
		shadow_read_write_fields,
		shadow_read_only_fields
	};
	const int max_fields[] = {
		max_shadow_read_write_fields,
		max_shadow_read_only_fields
	};
	struct shadow_vmcs_field field;
	unsigned long val;
	int i, q;

	for (q = 0; q < ARRAY_SIZE(fields); q++) {
		for (i = 0; i < max_fields[q]; i++) {
			field = fields[q][i];
			val = vmcs12_read_any(vmcs12, field.encoding,
					      field.offset);
			if (is_host_fields((field.encoding))) {
				pkvm_err("%s: field 0x%x is host field, please remove from shadowing!",
						__func__, field.encoding);
				continue;
			}
			__vmcs_writel(field.encoding, val);
		}
	}
}

/* current vmcs is vmcs01*/
static void save_vmcs01_fields_for_emulation(struct vcpu_vmx *vmx)
{
	vmx->vcpu.arch.efer = vmcs_read64(GUEST_IA32_EFER);
	vmx->vcpu.arch.pat = vmcs_read64(GUEST_IA32_PAT);
	vmx->vcpu.arch.dr7 = vmcs_readl(GUEST_DR7);
	vmx->nested.pre_vmenter_debugctl = vmcs_read64(GUEST_IA32_DEBUGCTL);
}

/* current vmcs is vmcs02*/
static u64 emulate_field_for_vmcs02(struct vcpu_vmx *vmx, u16 field, u64 virt_val)
{
	u64 val = virt_val;

	switch (field) {
	case VM_ENTRY_CONTROLS:
		/* L1 host wishes to use its own MSRs for L2 guest?
		 * emulate it by enabling vmentry load for such guest states
		 * then use vmcs01 saved guest states as vmcs02's guest states
		 */
		if ((val & VM_ENTRY_LOAD_IA32_EFER) != VM_ENTRY_LOAD_IA32_EFER)
			val |= VM_ENTRY_LOAD_IA32_EFER;
		if ((val & VM_ENTRY_LOAD_IA32_PAT) != VM_ENTRY_LOAD_IA32_PAT)
			val |= VM_ENTRY_LOAD_IA32_PAT;
		if ((val & VM_ENTRY_LOAD_DEBUG_CONTROLS) != VM_ENTRY_LOAD_DEBUG_CONTROLS)
			val |= VM_ENTRY_LOAD_DEBUG_CONTROLS;
		break;
	case VM_EXIT_CONTROLS:
		/* L1 host wishes to keep use MSRs from L2 guest after its VMExit?
		 * emulate it by enabling vmexit save for such guest states
		 * then vmcs01 shall take these guest states as its before L1 VMEntry
		 *
		 * And vmcs01 shall still keep enabling vmexit load such guest states as
		 * pkvm need restore from its host states
		 */
		if ((val & VM_EXIT_LOAD_IA32_EFER) != VM_EXIT_LOAD_IA32_EFER)
			val |= (VM_EXIT_LOAD_IA32_EFER | VM_EXIT_SAVE_IA32_EFER);
		if ((val & VM_EXIT_LOAD_IA32_PAT) != VM_EXIT_LOAD_IA32_PAT)
			val |= (VM_EXIT_LOAD_IA32_PAT | VM_EXIT_SAVE_IA32_PAT);
		/* host always in 64bit mode */
		val |= VM_EXIT_HOST_ADDR_SPACE_SIZE;
		break;
	case SECONDARY_VM_EXEC_CONTROL:
		val &= ~NESTED_UNSUPPORTED_2NDEXEC;
		break;
	}
	return val;
}

/* current vmcs is vmcs02*/
static void sync_vmcs12_dirty_fields_to_vmcs02(struct vcpu_vmx *vmx, struct vmcs12 *vmcs12)
{
	struct shadow_vmcs_field field;
	unsigned long val, phys_val;
	int i;

	if (vmx->nested.dirty_vmcs12) {
		for (i = 0; i < max_emulated_fields; i++) {
			field = emulated_fields[i];
			val = vmcs12_read_any(vmcs12, field.encoding, field.offset);
			phys_val = emulate_field_for_vmcs02(vmx, field.encoding, val);
			__vmcs_writel(field.encoding, phys_val);
		}
		vmx->nested.dirty_vmcs12 = false;
	}
}

/* current vmcs is vmcs02*/
static void update_vmcs02_fields_for_emulation(struct vcpu_vmx *vmx, struct vmcs12 *vmcs12)
{
	/* L1 host wishes to use its own MSRs for L2 guest?
	 * vmcs02 shall use such guest states in vmcs01 as its guest states
	 */
	if ((vmcs12->vm_entry_controls & VM_ENTRY_LOAD_IA32_EFER) != VM_ENTRY_LOAD_IA32_EFER)
		vmcs_write64(GUEST_IA32_EFER, vmx->vcpu.arch.efer);
	if ((vmcs12->vm_entry_controls & VM_ENTRY_LOAD_IA32_PAT) != VM_ENTRY_LOAD_IA32_PAT)
		vmcs_write64(GUEST_IA32_PAT, vmx->vcpu.arch.pat);
	if ((vmcs12->vm_entry_controls & VM_ENTRY_LOAD_DEBUG_CONTROLS) != VM_ENTRY_LOAD_DEBUG_CONTROLS) {
		vmcs_writel(GUEST_DR7, vmx->vcpu.arch.dr7);
		vmcs_write64(GUEST_IA32_DEBUGCTL, vmx->nested.pre_vmenter_debugctl);
	}
}

/* current vmcs is vmcs01, set vmcs01 guest state with vmcs02 host state */
static void prepare_vmcs01_guest_state(struct vcpu_vmx *vmx, struct vmcs12 *vmcs12)
{
	vmcs_writel(GUEST_CR0, vmcs12->host_cr0);
	vmcs_writel(GUEST_CR3, vmcs12->host_cr3);
	vmcs_writel(GUEST_CR4, vmcs12->host_cr4);

	vmcs_writel(GUEST_SYSENTER_ESP, vmcs12->host_ia32_sysenter_esp);
	vmcs_writel(GUEST_SYSENTER_EIP, vmcs12->host_ia32_sysenter_eip);
	vmcs_write32(GUEST_SYSENTER_CS, vmcs12->host_ia32_sysenter_cs);

	/* Both cases want vmcs01 to take EFER/PAT from L2
	 * 1. L1 host wishes to load its own MSRs on L2 guest VMExit
	 *    such vmcs12's host states shall be set as vmcs01's guest states
	 * 2. L1 host wishes to keep use MSRs from L2 guest after its VMExit
	 *    such vmcs02's guest state shall be set as vmcs01's guest states
	 *    the vmcs02's guest state were recorded in vmcs12 host
	 *
	 * For case 1, IA32_PERF_GLOBAL_CTRL is separately checked.
	 */
	vmcs_write64(GUEST_IA32_EFER, vmcs12->host_ia32_efer);
	vmcs_write64(GUEST_IA32_PAT, vmcs12->host_ia32_pat);
	if (vmcs12->vm_exit_controls & VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL)
		vmcs_write64(GUEST_IA32_PERF_GLOBAL_CTRL, vmcs12->host_ia32_perf_global_ctrl);

	vmcs_write16(GUEST_CS_SELECTOR, vmcs12->host_cs_selector);
	vmcs_write16(GUEST_DS_SELECTOR, vmcs12->host_ds_selector);
	vmcs_write16(GUEST_ES_SELECTOR, vmcs12->host_es_selector);
	vmcs_write16(GUEST_FS_SELECTOR, vmcs12->host_fs_selector);
	vmcs_write16(GUEST_GS_SELECTOR, vmcs12->host_gs_selector);
	vmcs_write16(GUEST_SS_SELECTOR, vmcs12->host_ss_selector);
	vmcs_write16(GUEST_TR_SELECTOR, vmcs12->host_tr_selector);

	vmcs_writel(GUEST_FS_BASE, vmcs12->host_fs_base);
	vmcs_writel(GUEST_GS_BASE, vmcs12->host_gs_base);
	vmcs_writel(GUEST_TR_BASE, vmcs12->host_tr_base);
	vmcs_writel(GUEST_GDTR_BASE, vmcs12->host_gdtr_base);
	vmcs_writel(GUEST_IDTR_BASE, vmcs12->host_idtr_base);

	vmcs_writel(GUEST_RIP, vmcs12->host_rip);
	vmcs_writel(GUEST_RSP, vmcs12->host_rsp);
	vmcs_writel(GUEST_RFLAGS, 0x2);
}

static void nested_release_vmcs12(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct pkvm_host_vcpu *pkvm_hvcpu = to_pkvm_hvcpu(vcpu);
	struct shadow_vcpu_state *cur_shadow_vcpu = pkvm_hvcpu->current_shadow_vcpu;
	struct vmcs *vmcs02;
	struct vmcs12 *vmcs12;

	if (vmx->nested.current_vmptr == INVALID_GPA)
		return;

	/* cur_shadow_vcpu must be valid here */
	vmcs02 = (struct vmcs *)cur_shadow_vcpu->vmcs02;
	vmcs12 = (struct vmcs12 *)cur_shadow_vcpu->cached_vmcs12;
	vmcs_load_track(vmx, vmcs02);
	copy_shadow_fields_vmcs02_to_vmcs12(vmx, vmcs12);

	vmcs_clear_track(vmx, vmcs02);
	clear_shadow_indicator(vmcs02);

	/*disable shadowing*/
	vmcs_load_track(vmx, vmx->loaded_vmcs->vmcs);
	secondary_exec_controls_clearbit(vmx, SECONDARY_EXEC_SHADOW_VMCS);
	vmcs_write64(VMCS_LINK_POINTER, INVALID_GPA);

	write_gpa(vcpu, vmx->nested.current_vmptr, vmcs12, VMCS12_SIZE);
	vmx->nested.dirty_vmcs12 = false;
	vmx->nested.current_vmptr = INVALID_GPA;
	pkvm_hvcpu->current_shadow_vcpu = NULL;

	put_shadow_vcpu(cur_shadow_vcpu->shadow_vcpu_handle);
}

static void nested_vmx_run(struct kvm_vcpu *vcpu, bool launch)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct pkvm_host_vcpu *pkvm_hvcpu = to_pkvm_hvcpu(vcpu);
	struct shadow_vcpu_state *cur_shadow_vcpu = pkvm_hvcpu->current_shadow_vcpu;
	struct vmcs *vmcs02 = (struct vmcs *)cur_shadow_vcpu->vmcs02;
	struct vmcs12 *vmcs12 = (struct vmcs12 *)cur_shadow_vcpu->cached_vmcs12;

	if (vmx->nested.current_vmptr == INVALID_GPA) {
		nested_vmx_result(VMfailInvalid, 0);
	} else if (vmcs12->launch_state == launch) {
		/* VMLAUNCH_NONCLEAR_VMCS or VMRESUME_NONLAUNCHED_VMCS */
		nested_vmx_result(VMfailValid,
			launch ? VMXERR_VMLAUNCH_NONCLEAR_VMCS : VMXERR_VMRESUME_NONLAUNCHED_VMCS);
	} else {
		/* save vmcs01 guest state for possible emulation */
		save_vmcs01_fields_for_emulation(vmx);

		/* switch to vmcs02 */
		vmcs_clear_track(vmx, vmcs02);
		clear_shadow_indicator(vmcs02);
		vmcs_load_track(vmx, vmcs02);

		sync_vmcs12_dirty_fields_to_vmcs02(vmx, vmcs12);

		update_vmcs02_fields_for_emulation(vmx, vmcs12);

		/* mark guest mode */
		vcpu->arch.hflags |= HF_GUEST_MASK;
	}
}

static void setup_guest_ept(struct shadow_vcpu_state *shadow_vcpu, u64 guest_eptp)
{
	struct vmcs12 *vmcs12 = (struct vmcs12 *)shadow_vcpu->cached_vmcs12;
	struct pkvm_shadow_vm *vm = shadow_vcpu->vm;
	bool invalidate = false;

	if (!is_valid_eptp(guest_eptp))
		pkvm_guest_ept_deinit(shadow_vcpu);
	else if (vmcs12->ept_pointer != guest_eptp) {
		pkvm_guest_ept_deinit(shadow_vcpu);
		pkvm_guest_ept_init(shadow_vcpu, guest_eptp);
	}

	pkvm_spin_lock(&vm->lock);
	if (vm->sept_desc.last_guest_eptp != guest_eptp) {
		vm->sept_desc.last_guest_eptp = guest_eptp;
		invalidate = true;
	}
	pkvm_spin_unlock(&vm->lock);

	if (invalidate)
		pkvm_invalidate_shadow_ept(&vm->sept_desc);
}

static int __do_handle_invept(struct pkvm_shadow_vm *vm, void *data)
{
	if (data) {
		/*
		 * Single context invalidate with specific guest eptp
		 */
		u64 guest_eptp = *(u64 *)data;
		struct shadow_vcpu_state *vcpu;
		struct vmcs12 *vmcs12;
		s64 shadow_vcpu_handle;
		int i;

		pkvm_spin_lock(&vm->lock);

		for (i = 0; i < vm->created_vcpus; i++) {
			shadow_vcpu_handle = to_shadow_vcpu_handle(vm->shadow_vm_handle, i);
			vcpu = get_shadow_vcpu(shadow_vcpu_handle);
			if (!vcpu)
				continue;
			vmcs12 = (struct vmcs12 *)vcpu->cached_vmcs12;
			if (vmcs12->ept_pointer == guest_eptp) {
				put_shadow_vcpu(shadow_vcpu_handle);
				break;
			}
			put_shadow_vcpu(shadow_vcpu_handle);
		}

		pkvm_spin_unlock(&vm->lock);

		/* No vCPU is using this guest eptp so no need invalidate */
		if (i == vm->created_vcpus)
			goto done;
	}

	/*
	 * Gloable context invalidation directly went here.
	 */
	pkvm_invalidate_shadow_ept(&vm->sept_desc);
done:
	return data ? 1 : 0;
}

static void do_handle_invept(u64 *guest_eptp)
{
	if (guest_eptp && !is_valid_eptp(*guest_eptp))
		return;

	for_each_valid_shadow_vm(__do_handle_invept, guest_eptp);
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
			vmx->nested.current_vmptr = INVALID_GPA;
			vmx->nested.dirty_vmcs12 = false;
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

int handle_vmptrld(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct pkvm_host_vcpu *pkvm_hvcpu = to_pkvm_hvcpu(vcpu);
	struct shadow_vcpu_state *shadow_vcpu;
	struct vmcs *vmcs02;
	struct vmcs12 *vmcs12;
	gpa_t vmptr;
	int r;

	if (check_vmx_permission(vcpu)) {
		if (nested_vmx_get_vmptr(vcpu, &vmptr, &r)) {
			nested_vmx_result(VMfailValid, VMXERR_VMPTRLD_INVALID_ADDRESS);
			return r;
		} else if (vmptr == vmx->nested.vmxon_ptr) {
			nested_vmx_result(VMfailValid, VMXERR_VMPTRLD_VMXON_POINTER);
		} else if (!validate_vmcs_revision_id(vcpu, vmptr)) {
			nested_vmx_result(VMfailValid, VMXERR_VMPTRLD_INCORRECT_VMCS_REVISION_ID);
		} else {
			if (vmx->nested.current_vmptr != vmptr) {
				s64 handle;

				nested_release_vmcs12(vcpu);

				handle = find_shadow_vcpu_handle_by_vmcs(vmptr);
				shadow_vcpu = handle > 0 ? get_shadow_vcpu(handle) : NULL;
				if ((handle > 0) && shadow_vcpu) {
					vmcs02 = (struct vmcs *)shadow_vcpu->vmcs02;
					vmcs12 = (struct vmcs12 *) shadow_vcpu->cached_vmcs12;

					read_gpa(vcpu, vmptr, vmcs12, VMCS12_SIZE);
					vmx->nested.dirty_vmcs12 = true;

					if (!shadow_vcpu->vmcs02_inited) {
						memset(vmcs02, 0, vmx_basic_vmcs_size(pkvm_hyp->vmcs_config.basic));
						vmcs02->hdr.revision_id = vmx_basic_vmcs_revision_id(pkvm_hyp->vmcs_config.basic);
						vmcs_load_track(vmx, vmcs02);
						pkvm_init_host_state_area(pkvm_hvcpu->pcpu, vcpu->cpu);
						vmcs_writel(HOST_RIP, (unsigned long)__pkvm_vmx_vmexit);
						shadow_vcpu->last_cpu = vcpu->cpu;
						shadow_vcpu->vmcs02_inited = true;
					} else {
						vmcs_load_track(vmx, vmcs02);
						if (shadow_vcpu->last_cpu != vcpu->cpu) {
							pkvm_init_host_state_area(pkvm_hvcpu->pcpu, vcpu->cpu);
							shadow_vcpu->last_cpu = vcpu->cpu;
						}
					}
					copy_shadow_fields_vmcs12_to_vmcs02(vmx, vmcs12);
					sync_vmcs12_dirty_fields_to_vmcs02(vmx, vmcs12);
					vmcs_clear_track(vmx, vmcs02);
					set_shadow_indicator(vmcs02);

					/* enable shadowing */
					vmcs_load_track(vmx, vmx->loaded_vmcs->vmcs);
					vmcs_write64(VMREAD_BITMAP, __pkvm_pa_symbol(vmx_vmread_bitmap));
					vmcs_write64(VMWRITE_BITMAP, __pkvm_pa_symbol(vmx_vmwrite_bitmap));
					secondary_exec_controls_setbit(vmx, SECONDARY_EXEC_SHADOW_VMCS);
					vmcs_write64(VMCS_LINK_POINTER, __pkvm_pa(vmcs02));

					vmx->nested.current_vmptr = vmptr;
					pkvm_hvcpu->current_shadow_vcpu = shadow_vcpu;

					nested_vmx_result(VMsucceed, 0);
				} else {
					nested_vmx_result(VMfailValid, VMXERR_VMPTRLD_INVALID_ADDRESS);
				}
			} else {
				nested_vmx_result(VMsucceed, 0);
			}
		}
	}

	return 0;
}

int handle_vmclear(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	gpa_t vmptr;
	u32 zero = 0;
	int r;

	if (check_vmx_permission(vcpu)) {
		if (nested_vmx_get_vmptr(vcpu, &vmptr, &r)) {
			nested_vmx_result(VMfailValid, VMXERR_VMPTRLD_INVALID_ADDRESS);
			return r;
		} else if (vmptr == vmx->nested.vmxon_ptr) {
			nested_vmx_result(VMfailValid, VMXERR_VMCLEAR_VMXON_POINTER);
		} else {
			if (vmx->nested.current_vmptr == vmptr)
				nested_release_vmcs12(vcpu);

			write_gpa(vcpu, vmptr + offsetof(struct vmcs12, launch_state),
					&zero, sizeof(zero));

			nested_vmx_result(VMsucceed, 0);
		}
	}

	return 0;
}

int handle_vmwrite(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct pkvm_host_vcpu *pkvm_hvcpu = to_pkvm_hvcpu(vcpu);
	struct shadow_vcpu_state *cur_shadow_vcpu = pkvm_hvcpu->current_shadow_vcpu;
	struct vmcs12 *vmcs12 = (struct vmcs12 *)cur_shadow_vcpu->cached_vmcs12;
	u32 instr_info = vmcs_read32(VMX_INSTRUCTION_INFO);
	struct x86_exception e;
	unsigned long field;
	short offset;
	gva_t gva;
	int r, reg;
	u64 value = 0;

	if (check_vmx_permission(vcpu)) {
		if (vmx->nested.current_vmptr == INVALID_GPA) {
			nested_vmx_result(VMfailInvalid, 0);
		} else {
			if (instr_info & BIT(10)) {
				reg = ((instr_info) >> 3) & 0xf;
				value = vcpu->arch.regs[reg];
			} else {
				if (get_vmx_mem_address(vcpu, vmx->exit_qualification,
							instr_info, &gva))
					return 1;

				r = read_gva(vcpu, gva, &value, 8, &e);
				if (r < 0) {
					/*TODO: handle memory failure exception */
					return r;
				}
			}

			reg = ((instr_info) >> 28) & 0xf;
			field = vcpu->arch.regs[reg];

			offset = get_vmcs12_field_offset(field);
			if (offset < 0) {
				nested_vmx_result(VMfailInvalid, VMXERR_UNSUPPORTED_VMCS_COMPONENT);
				return 0;
			}

			/*TODO: check vcpu supports "VMWRITE to any supported field in the VMCS"*/
			if (vmcs_field_readonly(field)) {
				nested_vmx_result(VMfailInvalid, VMXERR_VMWRITE_READ_ONLY_VMCS_COMPONENT);
				return 0;
			}

			/*
			 * Some Intel CPUs intentionally drop the reserved bits of the AR byte
			 * fields on VMWRITE.  Emulate this behavior to ensure consistent KVM
			 * behavior regardless of the underlying hardware, e.g. if an AR_BYTE
			 * field is intercepted for VMWRITE but not VMREAD (in L1), then VMREAD
			 * from L1 will return a different value than VMREAD from L2 (L1 sees
			 * the stripped down value, L2 sees the full value as stored by KVM).
			 */
			if (field >= GUEST_ES_AR_BYTES && field <= GUEST_TR_AR_BYTES)
				value &= 0x1f0ff;

			if (field == EPT_POINTER)
				setup_guest_ept(cur_shadow_vcpu, value);

			vmcs12_write_any(vmcs12, field, offset, value);

			if (is_emulated_fields(field)) {
				vmx->nested.dirty_vmcs12 = true;
				nested_vmx_result(VMsucceed, 0);
			} else if (is_host_fields(field)) {
				nested_vmx_result(VMsucceed, 0);
			} else {
				pkvm_err("%s: not include emulated fields 0x%lx, please add!\n",
						__func__, field);
				nested_vmx_result(VMfailInvalid, VMXERR_UNSUPPORTED_VMCS_COMPONENT);
			}
		}
	}

	return 0;
}

int handle_vmread(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct pkvm_host_vcpu *pkvm_hvcpu = to_pkvm_hvcpu(vcpu);
	struct shadow_vcpu_state *cur_shadow_vcpu = pkvm_hvcpu->current_shadow_vcpu;
	struct vmcs12 *vmcs12 = (struct vmcs12 *)cur_shadow_vcpu->cached_vmcs12;
	u32 instr_info = vmcs_read32(VMX_INSTRUCTION_INFO);
	struct x86_exception e;
	unsigned long field;
	short offset;
	gva_t gva = 0;
	int r, reg;
	u64 value;

	if (check_vmx_permission(vcpu)) {
		if (vmx->nested.current_vmptr == INVALID_GPA) {
			nested_vmx_result(VMfailInvalid, 0);
		} else {
			/* Decode instruction info and find the field to read */
			reg = ((instr_info) >> 28) & 0xf;
			field = vcpu->arch.regs[reg];

			offset = get_vmcs12_field_offset(field);
			if (offset < 0) {
				nested_vmx_result(VMfailInvalid, VMXERR_UNSUPPORTED_VMCS_COMPONENT);
			} else {
				value = vmcs12_read_any(vmcs12, field, offset);
				if (instr_info & BIT(10)) {
					reg = ((instr_info) >> 3) & 0xf;
					vcpu->arch.regs[reg] = value;
				} else {
					if (get_vmx_mem_address(vcpu, vmx->exit_qualification,
								instr_info, &gva))
						return 1;

					r = write_gva(vcpu, gva, &value, 8, &e);
					if (r < 0) {
						/*TODO: handle memory failure exception */
						return r;
					}
				}
				nested_vmx_result(VMsucceed, 0);
			}
		}
	}

	return 0;
}

int handle_vmresume(struct kvm_vcpu *vcpu)
{
	if (check_vmx_permission(vcpu))
		nested_vmx_run(vcpu, false);

	return 0;
}

int handle_vmlaunch(struct kvm_vcpu *vcpu)
{
	if (check_vmx_permission(vcpu))
		nested_vmx_run(vcpu, true);

	return 0;
}

int handle_invept(struct kvm_vcpu *vcpu)
{
	struct vmx_capability *vmx_cap = &pkvm_hyp->vmx_cap;
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	u32 vmx_instruction_info, types;
	unsigned long type;
	int gpr_index;

	if (!vmx_has_invept())
		/* TODO: inject #UD */
		return -EINVAL;

	if (!check_vmx_permission(vcpu))
		return 0;

	vmx_instruction_info = vmcs_read32(VMX_INSTRUCTION_INFO);
	gpr_index = vmx_get_instr_info_reg2(vmx_instruction_info);
	type = vcpu->arch.regs[gpr_index];
	types = (vmx_cap->ept >> VMX_EPT_EXTENT_SHIFT) & 6;

	if (type >= 32 || !(types & (1 << type))) {
		nested_vmx_result(VMfailValid, VMXERR_INVALID_OPERAND_TO_INVEPT_INVVPID);
		return 0;
	}

	switch (type) {
	case VMX_EPT_EXTENT_CONTEXT: {
		struct x86_exception e;
		gva_t gva;
		struct {
			u64 eptp, gpa;
		} operand;

		if (get_vmx_mem_address(vcpu, vmx->exit_qualification,
					vmx_instruction_info, &gva))
			/* TODO: handle the decode failure */
			return -EINVAL;

		if (read_gva(vcpu, gva, &operand, sizeof(operand), &e) < 0)
			/*TODO: handle memory failure exception */
			return -EINVAL;

		do_handle_invept(&operand.eptp);
		break;
	}
	case VMX_EPT_EXTENT_GLOBAL:
		do_handle_invept(NULL);
		break;
	default:
		break;
	}

	nested_vmx_result(VMsucceed, 0);
	return 0;
}

static bool nested_handle_ept_violation(struct shadow_vcpu_state *shadow_vcpu,
					u64 l2_gpa, u64 exit_quali)
{
	enum sept_handle_ret ret = pkvm_handle_shadow_ept_violation(shadow_vcpu,
								    l2_gpa, exit_quali);
	bool handled = false;

	switch (ret) {
	case PKVM_INJECT_EPT_MISC: {
		/*
		 * Inject EPT_MISCONFIG vmexit reason if can directly modify
		 * the read-only fields. Otherwise still deliver EPT_VIOLATION
		 * for simplification.
		 */
		if (vmx_has_vmwrite_any_field())
			vmcs_write32(VM_EXIT_REASON, EXIT_REASON_EPT_MISCONFIG);
		break;
	}
	case PKVM_HANDLED:
		handled = true;
		break;
	default:
		break;
	}

	if (handled && (vmcs_read32(IDT_VECTORING_INFO_FIELD) &
			VECTORING_INFO_VALID_MASK))
		/* pending interrupt, back to kvm-high to inject */
		handled = false;

	return handled;
}

int nested_vmexit(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct pkvm_host_vcpu *pkvm_hvcpu = to_pkvm_hvcpu(vcpu);
	struct shadow_vcpu_state *cur_shadow_vcpu = pkvm_hvcpu->current_shadow_vcpu;
	struct vmcs *vmcs02 = (struct vmcs *)cur_shadow_vcpu->vmcs02;
	struct vmcs12 *vmcs12 = (struct vmcs12 *)cur_shadow_vcpu->cached_vmcs12;

	if ((vmx->exit_reason.full == EXIT_REASON_EPT_VIOLATION) &&
		nested_handle_ept_violation(cur_shadow_vcpu,
					    vmcs_read64(GUEST_PHYSICAL_ADDRESS),
					    vmx->exit_qualification))
		/* EPT violation can be handled by pkvm, no need back to kvm-high */
		return 0;

	/* clear guest mode if need switch back to host */
	vcpu->arch.hflags &= ~HF_GUEST_MASK;

	/* L1 host wishes to keep use MSRs from L2 guest after its VMExit?
	 * save vmcs02 guest state for later vmcs01 guest state preparation
	 */
	if ((vmcs12->vm_exit_controls & VM_EXIT_LOAD_IA32_EFER) != VM_EXIT_LOAD_IA32_EFER)
		vmcs12->host_ia32_efer = vmcs_read64(GUEST_IA32_EFER);
	if ((vmcs12->vm_exit_controls & VM_EXIT_LOAD_IA32_PAT) != VM_EXIT_LOAD_IA32_PAT)
		vmcs12->host_ia32_pat = vmcs_read64(GUEST_IA32_PAT);

	if (!vmcs12->launch_state)
		vmcs12->launch_state = 1;

	/* switch to vmcs01 */
	vmcs_clear_track(vmx, vmcs02);
	set_shadow_indicator(vmcs02);
	vmcs_load_track(vmx, vmx->loaded_vmcs->vmcs);

	prepare_vmcs01_guest_state(vmx, vmcs12);

	return 0;
}

void pkvm_init_nest(void)
{
	init_vmcs_shadow_fields();
	init_emulated_vmcs_fields();
}
