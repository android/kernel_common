// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_HYP_H
#define __PKVM_HYP_H

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

#define PKVM_MAX_NORMAL_VM_NUM		8
#define PKVM_MAX_PROTECTED_VM_NUM	2

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

	/* The donated size of shadow_vcpu. */
	unsigned long shadow_size;

	struct hlist_node hnode;
	unsigned long vmcs12_pa;
	bool vmcs02_inited;

	struct vcpu_vmx vmx;

	/* represents for the virtual EPT configured by kvm-high */
	struct pkvm_pgtable vept;

	/* assume vmcs02 is one page */
	u8 vmcs02[PAGE_SIZE] __aligned(PAGE_SIZE);
	u8 cached_vmcs12[VMCS12_SIZE] __aligned(PAGE_SIZE);

	struct pkvm_ve_info ve_info;

	/* The last cpu this vmcs02 runs with */
	int last_cpu;

	/* point to the kvm_vcpu associated with this shadow_vcpu */
	struct kvm_vcpu *vcpu;
} __aligned(PAGE_SIZE);

#define SHADOW_VM_HANDLE_SHIFT		32
#define SHADOW_VCPU_INDEX_MASK		((1UL << SHADOW_VM_HANDLE_SHIFT) - 1)
#define to_shadow_vcpu_handle(vm_handle, vcpu_idx)		\
		(((s64)(vm_handle) << SHADOW_VM_HANDLE_SHIFT) | \
		 ((vcpu_idx) & SHADOW_VCPU_INDEX_MASK))

/*
 * Shadow_vcpu_array will be appended to the end of the pkvm_shadow_vm area
 * implicitly, so that the shadow_vcpu_state pointer cannot be got directly
 * from the pkvm_shadow_vm, but needs to be done through the interface
 * get/put_shadow_vcpu. This can prevent the shadow_vcpu_state pointer from
 * being abused without getting/putting the refcount.
 */
struct shadow_vcpu_array {
	struct shadow_vcpu_ref {
		atomic_t refcount;
		struct shadow_vcpu_state *vcpu;
	} ref[KVM_MAX_VCPUS];
} __aligned(PAGE_SIZE);

static inline size_t pkvm_shadow_vcpu_array_size(void)
{
	return sizeof(struct shadow_vcpu_array);
}

/*
 * Holds the relevant data for running a protected vm.
 */
struct pkvm_shadow_vm {
	/* A unique id to the shadow structs in the hyp shadow area. */
	int shadow_vm_handle;

	/* Number of vcpus for the vm. */
	int created_vcpus;

	/* The host's kvm va. */
	unsigned long host_kvm_va;

	/* The donated size of shadow_vm. */
	unsigned long shadow_size;

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

	/* The vm_type to indicate if this is a protected VM */
	u8 vm_type;

	pkvm_spinlock_t lock;
} __aligned(PAGE_SIZE);

#define sept_to_shadow_ept_desc(_sept)	container_of(_sept, struct shadow_ept_desc, sept)

#define sept_desc_to_shadow_vm(desc) container_of(desc, struct pkvm_shadow_vm, sept_desc)

#define sept_to_shadow_vm(_sept) sept_desc_to_shadow_vm(sept_to_shadow_ept_desc(_sept))

#define pgstate_pgt_to_shadow_vm(_pgt) container_of(_pgt, struct pkvm_shadow_vm, pgstate_pgt)

int __pkvm_init_shadow_vm(struct kvm_vcpu *hvcpu, unsigned long kvm_va,
			  unsigned long shadow_pa,  size_t shadow_size);
unsigned long __pkvm_teardown_shadow_vm(int shadow_vm_handle);
struct pkvm_shadow_vm *get_shadow_vm(int shadow_vm_handle);
void put_shadow_vm(int shadow_vm_handle);
void pkvm_shadow_vm_link_ptdev(struct pkvm_shadow_vm *vm,
			       struct list_head *node, bool coherency);
void pkvm_shadow_vm_unlink_ptdev(struct pkvm_shadow_vm *vm,
				 struct list_head *node, bool coherency);
s64 __pkvm_init_shadow_vcpu(struct kvm_vcpu *hvcpu, int shadow_vm_handle,
			    unsigned long vcpu_va, unsigned long shadow_pa,
			    size_t shadow_size);
unsigned long __pkvm_teardown_shadow_vcpu(s64 shadow_vcpu_handle);
struct shadow_vcpu_state *get_shadow_vcpu(s64 shadow_vcpu_handle);
void put_shadow_vcpu(s64 shadow_vcpu_handle);
s64 find_shadow_vcpu_handle_by_vmcs(unsigned long vmcs12_pa);
void pkvm_kick_vcpu(struct kvm_vcpu *vcpu);
int pkvm_add_ptdev(int shadow_vm_handle, u16 bdf, u32 pasid);

#define PKVM_REQ_TLB_FLUSH_HOST_EPT			KVM_ARCH_REQ(0)
#define PKVM_REQ_TLB_FLUSH_SHADOW_EPT			KVM_ARCH_REQ(1)

extern struct pkvm_hyp *pkvm_hyp;

static inline bool shadow_vcpu_is_protected(struct shadow_vcpu_state *shadow_vcpu)
{
	return shadow_vcpu->vm->vm_type == KVM_X86_PROTECTED_VM;
}

#endif
