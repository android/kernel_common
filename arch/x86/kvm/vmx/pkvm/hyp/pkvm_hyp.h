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

	struct vcpu_vmx vmx;

	/* represents for the virtual EPT configured by kvm-high */
	struct pkvm_pgtable vept;

	/* assume vmcs02 is one page */
	u8 vmcs02[PAGE_SIZE] __aligned(PAGE_SIZE);
	u8 cached_vmcs12[VMCS12_SIZE] __aligned(PAGE_SIZE);

	/* The last cpu this vmcs02 runs with */
	int last_cpu;
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

	/*
	 * VM's shadow EPT. All vCPU shares one mapping.
	 * FIXME: a potential security issue if some vCPUs are
	 * in SMM but the others are not.
	 */
	struct shadow_ept_desc sept_desc;

	pkvm_spinlock_t lock;
} __aligned(PAGE_SIZE);

#define sept_desc_to_shadow_vm(desc) container_of(desc, struct pkvm_shadow_vm, sept_desc)

int __pkvm_init_shadow_vm(unsigned long kvm_va, unsigned long shadow_pa,
			  size_t shadow_size);
unsigned long __pkvm_teardown_shadow_vm(int shadow_vm_handle);
s64 __pkvm_init_shadow_vcpu(struct kvm_vcpu *hvcpu, int shadow_vm_handle,
			    unsigned long vcpu_va, unsigned long shadow_pa,
			    size_t shadow_size);
unsigned long __pkvm_teardown_shadow_vcpu(s64 shadow_vcpu_handle);
struct shadow_vcpu_state *get_shadow_vcpu(s64 shadow_vcpu_handle);
void put_shadow_vcpu(s64 shadow_vcpu_handle);
s64 find_shadow_vcpu_handle_by_vmcs(unsigned long vmcs12_pa);

extern struct pkvm_hyp *pkvm_hyp;

#endif
