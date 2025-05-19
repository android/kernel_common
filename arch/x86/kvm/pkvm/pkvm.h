/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_PKVM_H
#define __PKVM_X86_PKVM_H

#include <asm/kvm_host.h>
#include <asm/pkvm_spinlock.h>

#define PKVM_MAX_NORMAL_VM_NUM		8
#define PKVM_MAX_PROTECTED_VM_NUM	2
#define MAX_PKVM_VMS			(PKVM_MAX_NORMAL_VM_NUM + PKVM_MAX_PROTECTED_VM_NUM)

DECLARE_PER_CPU(struct kvm_vcpu *, host_vcpu);

extern size_t pkvm_vm_sz;
extern size_t pkvm_vcpu_sz;

/*
 * Struct kvm_vcpu can be appended in the end of pkvm_vcpu as below:
 *  ---------------------
 *  | struct pkvm_vcpu  |
 *  ---------------------
 *  | struct kvm_vcpu   |
 *  ---------------------
 *
 * The reason of *NOT* explicitly putting struct kvm_vcpu inside the pkvm_vcpu
 * is that the struct kvm may be wrapped by the architecture specific structure
 * e.g. struct vcpu_vmx. Appending struct kvm_vcpu in the end is friendly to
 * this case as the architecture specific structure can be appended in the end
 * if the struct kvm_vcpu is wrapped at the offset 0.
 */
struct pkvm_vcpu {
	/* Pointer to the shared structure kvm_vcpu in host */
	struct kvm_vcpu *shared_vcpu;
	/* The donated structure size */
	size_t size;
	/* Vcpu index inside the vm */
	int vcpu_idx;
	/* Point to the pkvm_vm this pkvm_vcpu belongs to */
	struct pkvm_vm *pkvm_vm;
	/* Requests for the host VMM to handle */
	unsigned long reqs_to_host;
	/* The host emulated MSR error */
	int host_emulated_msr_err;
} __aligned(PAGE_SIZE);

/*
 * Struct kvm can be appended in the end of pkvm_vm as below:
 *  -------------------
 *  | struct pkvm_vm  |
 *  -------------------
 *  | struct kvm      |
 *  -------------------
 *
 * The reason of *NOT* explicitly putting struct kvm inside the pkvm_vm is that
 * the struct kvm may be wrapped by the architecture specific structure, e.g.
 * struct kvm_vmx. Appending struct kvm in the end is friendly to this case as
 * the architecture specific structure can be appended in the end if the struct
 * kvm is wrapped at the offset 0.
 */
struct pkvm_vm {
	/* Point to the kvm structure in host */
	struct kvm *shared_kvm;
	/* Structure size */
	size_t size;

	pkvm_spinlock_t lock;

	struct pkvm_vcpu *vcpus[KVM_MAX_VCPUS];
};

struct pkvm_vm_ref {
	/* Reference counter to indicate if pkvm_vm is inuse */
	atomic_t refcount;
	/* Point to pkvm_vm in pkvm */
	struct pkvm_vm *pkvm_vm;
};

static inline struct kvm *to_kvm(struct pkvm_vm *pkvm_vm)
{
	/* See comments for pkvm_vm */
	return (struct kvm *)((unsigned long)pkvm_vm + sizeof(struct pkvm_vm));
}

static inline struct pkvm_vm *to_pkvm(struct kvm *kvm)
{
	/* See comments for pkvm_vm */
	return (struct pkvm_vm *)((unsigned long)kvm - sizeof(struct pkvm_vm));
}

static inline struct kvm_vcpu *to_kvm_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	/* See comments for pkvm_vcpu */
	return (struct kvm_vcpu *)((unsigned long)pkvm_vcpu + sizeof(struct pkvm_vcpu));
}

static inline struct pkvm_vcpu *to_pkvm_vcpu(struct kvm_vcpu *vcpu)
{
	/* See comments for pkvm_vcpu */
	return (struct pkvm_vcpu *)((unsigned long)vcpu - sizeof(struct pkvm_vcpu));
}

static inline void pkvm_make_req_to_host(int req, struct kvm_vcpu *vcpu)
{
	BUILD_BUG_ON(req >= sizeof(to_pkvm_vcpu(vcpu)->reqs_to_host) * 8);

	set_bit(req, &to_pkvm_vcpu(vcpu)->reqs_to_host);
}

static inline void pkvm_reset_reqs_to_host(struct kvm_vcpu *vcpu)
{
	to_pkvm_vcpu(vcpu)->reqs_to_host = 0;
}

static inline unsigned long pkvm_reqs_to_host(struct kvm_vcpu *vcpu)
{
	return to_pkvm_vcpu(vcpu)->reqs_to_host;
}

static inline bool pkvm_has_req_to_host(int req, struct kvm_vcpu *vcpu)
{
	BUILD_BUG_ON(req >= sizeof(to_pkvm_vcpu(vcpu)->reqs_to_host) * 8);

	return test_bit(req, &to_pkvm_vcpu(vcpu)->reqs_to_host);
}

/*
 * Convert a linux host kernel direct mapping virtual address to pKVM mapping
 * virtual address. Currently the two virtual address are the same.
 */
static inline void *kern_pkvm_va(void *va)
{
	return va;
}

struct pkvm_x86_ops {
	void (*switch_to_guest_vcpu)(struct kvm_vcpu *vcpu);
	void (*switch_to_host_vcpu)(struct kvm_vcpu *vcpu);
	void (*sync_vcpu_state_post_switch)(struct pkvm_vcpu *pkvm_vcpu);
	void (*sync_vcpu_state_pre_switch)(struct pkvm_vcpu *pkvm_vcpu);
	/*
	 * FIMXE: This is to support the shadow EPT usage. Should be revisited
	 * when the PV EPT is ready.
	 */
	void (*setup_virtual_mmu)(struct kvm_vcpu *vcpu, hpa_t root_hpa, int root_level);
};

extern struct pkvm_x86_ops pkvm_x86_ops;

#define pkvm_x86_call(func)				\
({							\
	BUG_ON(!pkvm_x86_ops.func);			\
	(pkvm_x86_ops.func);				\
})

struct pkvm_vm *get_pkvm_vm(int handle);
void put_pkvm_vm(struct pkvm_vm *pkvm_vm);
struct pkvm_vcpu *get_pkvm_vcpu(int vm_handle, int vcpu_handle);
void put_pkvm_vcpu(struct pkvm_vcpu *pkvm_vcpu);
unsigned long handle_kvm_call(unsigned long fn, unsigned long p1,
			      unsigned long p2, unsigned long p3);
void pkvm_x86_ops_init(struct pkvm_x86_ops *ops);

static inline bool pkvm_vm_has_pvmfw(struct kvm *kvm)
{
	return kvm->arch.pkvm.pvmfw_load_addr != INVALID_GPA;
}

static inline bool pkvm_vcpu_is_pvmfw_bsp(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_is_reset_bsp(vcpu) && pkvm_vm_has_pvmfw(vcpu->kvm);
}

#endif /* __PKVM_X86_PKVM_H */
