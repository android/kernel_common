// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_HYP_TYPES_H
#define __PKVM_HYP_TYPES_H

#include <asm/pkvm_spinlock.h>
#include "pgtable.h"

/*
 * Descriptor for shadow EPT
 */
struct shadow_ept_desc {
	/* shadow EPTP value configured by pkvm */
	u64 shadow_eptp;

	/* Save the last guest EPTP value configured by kvm high */
	u64 last_guest_eptp;

	struct pkvm_pgtable sept;
};

/*
 * Store the Virtualization Exception(#VE) information when a #VE occurs. This
 * struture definition is based on
 * sdm Volume 3, 25.5.7.2 Virtualizaiton-Exception Information.
 */
struct pkvm_ve_info {
	u32 exit_reason;
	u32 valid;
	u64 exit_qual;
	u64 gla;
	u64 gpa;
	u16 eptp_index;
};

/*
 * A container for the vcpu state that hyp needs to maintain for protected VMs.
 */
struct shadow_vcpu_state {
	/*
	 * A unique id to the shadow vcpu, which is combined by
	 * shadow_vm_handle and shadow_vcpu index in the array.
	 * As shadow_vm_handle is in the high end and it is an
	 * int, so define the shadow_vcpu_handle as a s64.
	 */
	s64 shadow_vcpu_handle;

	struct pkvm_shadow_vm *vm;

	struct hlist_node hnode;
	unsigned long vmcs12_pa;
	bool vmcs02_inited;

	/* represents for the virtual EPT configured by kvm-high */
	struct pkvm_pgtable vept;

	struct vmcs *vmcs02;
	u8 cached_vmcs12[VMCS12_SIZE] __aligned(PAGE_SIZE);

	struct pkvm_ve_info ve_info __aligned(PAGE_SIZE);

	bool allowed_to_run;
	bool pvmfw_entry_pending;

	/* The last cpu this vmcs02 runs with */
	int last_cpu;

	/* point to the kvm_vcpu associated with this shadow_vcpu */
	struct kvm_vcpu *vcpu;
} __aligned(PAGE_SIZE);

/*
 * Holds the relevant data for running a protected vm.
 */
struct pkvm_shadow_vm {
	/*
	 * VM's shadow EPT. All vCPU shares one mapping.
	 * FIXME: a potential security issue if some vCPUs are
	 * in SMM but the others are not.
	 */
	struct shadow_ept_desc sept_desc;

	/*
	 * Page state page table manages the page states, and
	 * works as IOMMU second-level page table for protected
	 * VM with passthrough devices. For the protected VM
	 * without passthrough devices or normal VM, it manages
	 * the page states only.
	 */
	struct pkvm_pgtable pgstate_pgt;
	/* Indicate if pgstate_pgt needs to be prepopulated */
	bool need_prepopulation;
	/*
	 * Indicate the count of the shadow VM passthrough devices
	 * which are attached to non-coherent IOMMU.
	 */
	unsigned long noncoherent_ptdev;

	/* link the passthrough devices of a protected VM */
	struct list_head ptdev_head;

	/*
	 * Address where the pvmfw is loaded in a protected VM memory
	 * or PVMFW_INVALID_LOAD_ADDR if the VM is running without pvmfw
	 */
	gpa_t pvmfw_load_addr;

	/* Indicate if the VM and its vCPUs are set up and ready to run */
	bool finalized;

	/* The vm_type to indicate if this is a protected VM */
	u8 vm_type;

	pkvm_spinlock_t lock;
} __aligned(PAGE_SIZE);

#endif
