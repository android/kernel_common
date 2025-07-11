/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_FPU_H
#define __PKVM_X86_FPU_H

void pkvm_setup_xstate_cache(void);
void pkvm_init_percpu_fpu(void);

#endif /* __PKVM_X86_FPU_H */
