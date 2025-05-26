/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_SMP_H_
#define __PKVM_X86_SMP_H_

unsigned int pkvm_per_cpu_nr_pages(void);
int setup_pkvm_per_cpu(int cpu, unsigned long base);
void warn_thunk_thunk(void);

#endif
