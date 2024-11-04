/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_DEBUG_H_
#define __PKVM_X86_DEBUG_H_

#ifdef CONFIG_PKVM_INTEL_DEBUG

#define pkvm_debug_sym(sym) sym##__pkvm

void pkvm_debug_sym(__dynamic_pr_debug)(struct _ddebug *descriptor, const char *fmt, ...);
int pkvm_debug_sym(_printk)(const char *fmt, ...);
noinstr struct cpu_entry_area *pkvm_debug_sym(get_cpu_entry_area)(int cpu);
unsigned long pkvm_debug_sym(get_pcpu_id)(void);

#endif

#endif /* __PKVM_X86_DEBUG_H */
