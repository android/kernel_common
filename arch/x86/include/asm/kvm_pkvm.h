// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef _ASM_X86_KVM_PKVM_H
#define _ASM_X86_KVM_PKVM_H

#ifdef CONFIG_PKVM_INTEL

void *pkvm_phys_to_virt(unsigned long phys);
unsigned long pkvm_virt_to_phys(void *virt);

#define __pkvm_pa(virt)	pkvm_virt_to_phys((void *)(virt))
#define __pkvm_va(phys)	pkvm_phys_to_virt((unsigned long)(phys))

/*TODO: unify the API name: __pkvm vs. __hyp? */
#define __hyp_pa __pkvm_pa
#define __hyp_va __pkvm_va

#endif

#endif
