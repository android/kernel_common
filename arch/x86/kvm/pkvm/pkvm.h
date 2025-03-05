/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_PKVM_H
#define __PKVM_X86_PKVM_H

#include <asm/kvm_host.h>
#include <asm/pkvm_spinlock.h>

DECLARE_PER_CPU(struct kvm_vcpu *, host_vcpu);

extern size_t pkvm_vm_sz;
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

unsigned long handle_kvm_call(unsigned long fn, unsigned long p1,
			      unsigned long p2, unsigned long p3);

#endif /* __PKVM_X86_PKVM_H */
