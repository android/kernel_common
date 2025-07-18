/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_MMU_H
#define __PKVM_X86_MMU_H

#include "pkvm.h"
//FIXME: clean up the header files
#include <vmx/pkvm/hyp/pgtable.h>
#include <pkvm.h>

extern const struct pkvm_pgtable_ops *guest_mmu_pgt_ops;
extern struct pkvm_pgtable_cap guest_mmu_pgt_cap;

int pkvm_vm_mmu_init(struct pkvm_vm *pkvm_vm);
void pkvm_vm_mmu_destroy(struct pkvm_vm *pkvm_vm);
int pkvm_vm_mmu_map(struct kvm_vcpu *shared_vcpu, u64 gpa, u64 hpa, u64 size, bool writable);
int pkvm_vm_mmu_unmap(int vm_handle, u64 gpa, u64 size);
int pkvm_vm_mmu_age(int vm_handle, u64 gpa, u64 size, bool mkold);

int pkvm_refill_mmu_memcache(struct pkvm_vcpu *pkvm_vcpu);
void pkvm_free_mmu_memcache(struct kvm_vcpu *vcpu, struct pkvm_memcache *teardown_mc);

#endif /* __PKVM_X86_MMU_H */
