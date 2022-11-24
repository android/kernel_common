// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#if !defined(EMULATED_FIELD_RW) && !defined(SHADOW_FIELD_RW) && !defined(SHADOW_FIELD_RO)
BUILD_BUG_ON(1)
#endif

#ifndef EMULATED_FIELD_RW
#define EMULATED_FIELD_RW(x, y)
#endif
#ifndef SHADOW_FIELD_RW
#define SHADOW_FIELD_RW(x, y)
#endif
#ifndef SHADOW_FIELD_RO
#define SHADOW_FIELD_RO(x, y)
#endif

/*
 * Emulated fields for vmcs02:
 *
 * These fields are recorded in cached_vmcs12, and should be emulated to
 * real value in vmcs02 before vmcs01 active.
 */
/* 16-bits */
EMULATED_FIELD_RW(VIRTUAL_PROCESSOR_ID, virtual_processor_id)

/* 32-bits */
EMULATED_FIELD_RW(VM_EXIT_CONTROLS, vm_exit_controls)
EMULATED_FIELD_RW(VM_ENTRY_CONTROLS, vm_entry_controls)
EMULATED_FIELD_RW(SECONDARY_VM_EXEC_CONTROL, secondary_vm_exec_control)

/* 64-bits, what about their HIGH 32 fields?  */
EMULATED_FIELD_RW(IO_BITMAP_A, io_bitmap_a)
EMULATED_FIELD_RW(IO_BITMAP_B, io_bitmap_b)
EMULATED_FIELD_RW(MSR_BITMAP, msr_bitmap)
EMULATED_FIELD_RW(VM_EXIT_MSR_STORE_ADDR, vm_exit_msr_store_addr)
EMULATED_FIELD_RW(VM_EXIT_MSR_LOAD_ADDR, vm_exit_msr_load_addr)
EMULATED_FIELD_RW(VM_ENTRY_MSR_LOAD_ADDR, vm_entry_msr_load_addr)
EMULATED_FIELD_RW(XSS_EXIT_BITMAP, xss_exit_bitmap)
EMULATED_FIELD_RW(POSTED_INTR_DESC_ADDR, posted_intr_desc_addr)
EMULATED_FIELD_RW(PML_ADDRESS, pml_address)
EMULATED_FIELD_RW(VM_FUNCTION_CONTROL, vm_function_control)
EMULATED_FIELD_RW(EPT_POINTER, ept_pointer)
EMULATED_FIELD_RW(EOI_EXIT_BITMAP0, eoi_exit_bitmap0)
EMULATED_FIELD_RW(EOI_EXIT_BITMAP1, eoi_exit_bitmap1)
EMULATED_FIELD_RW(EOI_EXIT_BITMAP2, eoi_exit_bitmap2)
EMULATED_FIELD_RW(EOI_EXIT_BITMAP3, eoi_exit_bitmap3)
EMULATED_FIELD_RW(EPTP_LIST_ADDRESS, eptp_list_address)
EMULATED_FIELD_RW(VMREAD_BITMAP, vmread_bitmap)
EMULATED_FIELD_RW(VMWRITE_BITMAP, vmwrite_bitmap)
EMULATED_FIELD_RW(ENCLS_EXITING_BITMAP, encls_exiting_bitmap)
EMULATED_FIELD_RW(VMCS_LINK_POINTER, vmcs_link_pointer)

/*
 * Shadow fields for vmcs02:
 *
 * These fields are HW shadowing in vmcs02, we try to shadow all non-host
 * fields except emulated ones.
 * Host state fields need to be recorded in cached_vmcs12 and restored to vmcs01's
 * guest state when returning to L1 host, so please ensure __NO__ host fields below.
 */

/* 16-bits */
SHADOW_FIELD_RW(POSTED_INTR_NV, posted_intr_nv)
SHADOW_FIELD_RW(GUEST_ES_SELECTOR, guest_es_selector)
SHADOW_FIELD_RW(GUEST_CS_SELECTOR, guest_cs_selector)
SHADOW_FIELD_RW(GUEST_SS_SELECTOR, guest_ss_selector)
SHADOW_FIELD_RW(GUEST_DS_SELECTOR, guest_ds_selector)
SHADOW_FIELD_RW(GUEST_FS_SELECTOR, guest_fs_selector)
SHADOW_FIELD_RW(GUEST_GS_SELECTOR, guest_gs_selector)
SHADOW_FIELD_RW(GUEST_LDTR_SELECTOR, guest_ldtr_selector)
SHADOW_FIELD_RW(GUEST_TR_SELECTOR, guest_tr_selector)
SHADOW_FIELD_RW(GUEST_TR_SELECTOR, guest_tr_selector)
SHADOW_FIELD_RW(GUEST_INTR_STATUS, guest_intr_status)
SHADOW_FIELD_RW(GUEST_PML_INDEX, guest_pml_index)

/* 32-bits */
SHADOW_FIELD_RW(PIN_BASED_VM_EXEC_CONTROL, pin_based_vm_exec_control)
SHADOW_FIELD_RW(CPU_BASED_VM_EXEC_CONTROL, cpu_based_vm_exec_control)
SHADOW_FIELD_RW(EXCEPTION_BITMAP, exception_bitmap)
SHADOW_FIELD_RW(PAGE_FAULT_ERROR_CODE_MASK, page_fault_error_code_mask)
SHADOW_FIELD_RW(PAGE_FAULT_ERROR_CODE_MATCH, page_fault_error_code_match)
SHADOW_FIELD_RW(CR3_TARGET_COUNT, cr3_target_count)
SHADOW_FIELD_RW(VM_EXIT_MSR_STORE_COUNT, vm_exit_msr_store_count)
SHADOW_FIELD_RW(VM_EXIT_MSR_LOAD_COUNT, vm_exit_msr_load_count)
SHADOW_FIELD_RW(VM_ENTRY_MSR_LOAD_COUNT, vm_entry_msr_load_count)
SHADOW_FIELD_RW(VM_ENTRY_INTR_INFO_FIELD, vm_entry_intr_info_field)
SHADOW_FIELD_RW(VM_ENTRY_EXCEPTION_ERROR_CODE, vm_entry_exception_error_code)
SHADOW_FIELD_RW(VM_ENTRY_INSTRUCTION_LEN, vm_entry_instruction_len)
SHADOW_FIELD_RW(TPR_THRESHOLD, tpr_threshold)
SHADOW_FIELD_RW(GUEST_ES_LIMIT, guest_es_limit)
SHADOW_FIELD_RW(GUEST_CS_LIMIT, guest_cs_limit)
SHADOW_FIELD_RW(GUEST_SS_LIMIT, guest_ss_limit)
SHADOW_FIELD_RW(GUEST_DS_LIMIT, guest_ds_limit)
SHADOW_FIELD_RW(GUEST_FS_LIMIT, guest_fs_limit)
SHADOW_FIELD_RW(GUEST_GS_LIMIT, guest_gs_limit)
SHADOW_FIELD_RW(GUEST_LDTR_LIMIT, guest_ldtr_limit)
SHADOW_FIELD_RW(GUEST_TR_LIMIT, guest_tr_limit)
SHADOW_FIELD_RW(GUEST_GDTR_LIMIT, guest_gdtr_limit)
SHADOW_FIELD_RW(GUEST_IDTR_LIMIT, guest_idtr_limit)
SHADOW_FIELD_RW(GUEST_ES_AR_BYTES, guest_es_ar_bytes)
SHADOW_FIELD_RW(GUEST_CS_AR_BYTES, guest_cs_ar_bytes)
SHADOW_FIELD_RW(GUEST_SS_AR_BYTES, guest_ss_ar_bytes)
SHADOW_FIELD_RW(GUEST_DS_AR_BYTES, guest_ds_ar_bytes)
SHADOW_FIELD_RW(GUEST_FS_AR_BYTES, guest_fs_ar_bytes)
SHADOW_FIELD_RW(GUEST_GS_AR_BYTES, guest_gs_ar_bytes)
SHADOW_FIELD_RW(GUEST_LDTR_AR_BYTES, guest_ldtr_ar_bytes)
SHADOW_FIELD_RW(GUEST_TR_AR_BYTES, guest_tr_ar_bytes)
SHADOW_FIELD_RW(GUEST_INTERRUPTIBILITY_INFO, guest_interruptibility_info)
SHADOW_FIELD_RW(GUEST_ACTIVITY_STATE, guest_activity_state)
SHADOW_FIELD_RW(GUEST_SYSENTER_CS, guest_sysenter_cs)
SHADOW_FIELD_RW(VMX_PREEMPTION_TIMER_VALUE, vmx_preemption_timer_value)
SHADOW_FIELD_RW(PLE_GAP, ple_gap)
SHADOW_FIELD_RW(PLE_WINDOW, ple_window)

/* Natural width */
SHADOW_FIELD_RW(CR0_GUEST_HOST_MASK, cr0_guest_host_mask)
SHADOW_FIELD_RW(CR4_GUEST_HOST_MASK, cr4_guest_host_mask)
SHADOW_FIELD_RW(CR0_READ_SHADOW, cr0_read_shadow)
SHADOW_FIELD_RW(CR4_READ_SHADOW, cr4_read_shadow)
SHADOW_FIELD_RW(GUEST_CR0, guest_cr0)
SHADOW_FIELD_RW(GUEST_CR3, guest_cr3)
SHADOW_FIELD_RW(GUEST_CR4, guest_cr4)
SHADOW_FIELD_RW(GUEST_ES_BASE, guest_es_base)
SHADOW_FIELD_RW(GUEST_CS_BASE, guest_cs_base)
SHADOW_FIELD_RW(GUEST_SS_BASE, guest_ss_base)
SHADOW_FIELD_RW(GUEST_DS_BASE, guest_ds_base)
SHADOW_FIELD_RW(GUEST_FS_BASE, guest_fs_base)
SHADOW_FIELD_RW(GUEST_GS_BASE, guest_gs_base)
SHADOW_FIELD_RW(GUEST_LDTR_BASE, guest_ldtr_base)
SHADOW_FIELD_RW(GUEST_TR_BASE, guest_tr_base)
SHADOW_FIELD_RW(GUEST_GDTR_BASE, guest_gdtr_base)
SHADOW_FIELD_RW(GUEST_IDTR_BASE, guest_idtr_base)
SHADOW_FIELD_RW(GUEST_DR7, guest_dr7)
SHADOW_FIELD_RW(GUEST_RSP, guest_rsp)
SHADOW_FIELD_RW(GUEST_RIP, guest_rip)
SHADOW_FIELD_RW(GUEST_RFLAGS, guest_rflags)
SHADOW_FIELD_RW(GUEST_PENDING_DBG_EXCEPTIONS, guest_pending_dbg_exceptions)
SHADOW_FIELD_RW(GUEST_SYSENTER_ESP, guest_sysenter_esp)
SHADOW_FIELD_RW(GUEST_SYSENTER_EIP, guest_sysenter_eip)

/* 64-bit */
SHADOW_FIELD_RW(TSC_OFFSET, tsc_offset)
SHADOW_FIELD_RW(TSC_OFFSET_HIGH, tsc_offset)
SHADOW_FIELD_RW(VIRTUAL_APIC_PAGE_ADDR, virtual_apic_page_addr)
SHADOW_FIELD_RW(VIRTUAL_APIC_PAGE_ADDR_HIGH, virtual_apic_page_addr)
SHADOW_FIELD_RW(APIC_ACCESS_ADDR, apic_access_addr)
SHADOW_FIELD_RW(APIC_ACCESS_ADDR_HIGH, apic_access_addr)
SHADOW_FIELD_RW(TSC_MULTIPLIER, tsc_multiplier)
SHADOW_FIELD_RW(TSC_MULTIPLIER_HIGH, tsc_multiplier)
SHADOW_FIELD_RW(GUEST_IA32_DEBUGCTL, guest_ia32_debugctl)
SHADOW_FIELD_RW(GUEST_IA32_DEBUGCTL_HIGH, guest_ia32_debugctl)
SHADOW_FIELD_RW(GUEST_IA32_PAT, guest_ia32_pat)
SHADOW_FIELD_RW(GUEST_IA32_PAT_HIGH, guest_ia32_pat)
SHADOW_FIELD_RW(GUEST_IA32_EFER, guest_ia32_efer)
SHADOW_FIELD_RW(GUEST_IA32_EFER_HIGH, guest_ia32_efer)
SHADOW_FIELD_RW(GUEST_IA32_PERF_GLOBAL_CTRL, guest_ia32_perf_global_ctrl)
SHADOW_FIELD_RW(GUEST_IA32_PERF_GLOBAL_CTRL_HIGH, guest_ia32_perf_global_ctrl)
SHADOW_FIELD_RW(GUEST_PDPTR0, guest_pdptr0)
SHADOW_FIELD_RW(GUEST_PDPTR0_HIGH, guest_pdptr0)
SHADOW_FIELD_RW(GUEST_PDPTR1, guest_pdptr1)
SHADOW_FIELD_RW(GUEST_PDPTR1_HIGH, guest_pdptr1)
SHADOW_FIELD_RW(GUEST_PDPTR2, guest_pdptr2)
SHADOW_FIELD_RW(GUEST_PDPTR2_HIGH, guest_pdptr2)
SHADOW_FIELD_RW(GUEST_PDPTR3, guest_pdptr3)
SHADOW_FIELD_RW(GUEST_PDPTR3_HIGH, guest_pdptr3)
SHADOW_FIELD_RW(GUEST_BNDCFGS, guest_bndcfgs)
SHADOW_FIELD_RW(GUEST_BNDCFGS_HIGH, guest_bndcfgs)

/* 32-bits */
SHADOW_FIELD_RO(VM_INSTRUCTION_ERROR, vm_instruction_error)
SHADOW_FIELD_RO(VM_EXIT_REASON, vm_exit_reason)
SHADOW_FIELD_RO(VM_EXIT_INTR_INFO, vm_exit_intr_info)
SHADOW_FIELD_RO(VM_EXIT_INTR_ERROR_CODE, vm_exit_intr_error_code)
SHADOW_FIELD_RO(IDT_VECTORING_INFO_FIELD, idt_vectoring_info_field)
SHADOW_FIELD_RO(IDT_VECTORING_ERROR_CODE, idt_vectoring_error_code)
SHADOW_FIELD_RO(VM_EXIT_INSTRUCTION_LEN, vm_exit_instruction_len)
SHADOW_FIELD_RO(VMX_INSTRUCTION_INFO, vmx_instruction_info)

/* Natural width */
SHADOW_FIELD_RO(EXIT_QUALIFICATION, exit_qualification)
SHADOW_FIELD_RO(EXIT_IO_RCX, exit_io_rcx)
SHADOW_FIELD_RO(EXIT_IO_RSI, exit_io_rsi)
SHADOW_FIELD_RO(EXIT_IO_RDI, exit_io_rdi)
SHADOW_FIELD_RO(EXIT_IO_RIP, exit_io_rip)
SHADOW_FIELD_RO(GUEST_LINEAR_ADDRESS, guest_linear_address)

/* 64-bit */
SHADOW_FIELD_RO(GUEST_PHYSICAL_ADDRESS, guest_physical_address)
SHADOW_FIELD_RO(GUEST_PHYSICAL_ADDRESS_HIGH, guest_physical_address)

#undef EMULATED_FIELD_RW
#undef SHADOW_FIELD_RW
#undef SHADOW_FIELD_RO
