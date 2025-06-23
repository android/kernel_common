/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_MMU_H
#define __PKVM_X86_MMU_H

//FIXME: clean up the header files
#include <vmx/pkvm/hyp/pgtable.h>
#include <pkvm.h>

extern const struct pkvm_pgtable_ops *guest_mmu_pgt_ops;
extern struct pkvm_pgtable_cap guest_mmu_pgt_cap;

#endif /* __PKVM_X86_MMU_H */
