/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_DEF_H
#define __PKVM_X86_DEF_H

/*
 * Special hack: pKVM runs in the highest privilege level, which is higher than
 * the linux kernel. This means that pKVM cannot use any of the linux kernel
 * symbols. To make pKVM being able to use the linux kernel headers without
 * introducing additional symbols, some kernel configuration options are
 * disabled. (This list needs to be extended when new variants are added.)
 */
#undef CONFIG_DEBUG_PREEMPT
#ifndef CONFIG_PKVM_INTEL_DEBUG
#undef CONFIG_PRINTK
#undef CONFIG_DYNAMIC_DEBUG
#undef CONFIG_DYNAMIC_DEBUG_CORE
#undef CONFIG_TRACING
#undef CONFIG_BUG
#undef CONFIG_GENERIC_BUG
#endif
#undef CONFIG_PARAVIRT
#undef CONFIG_PARAVIRT_XXL
#undef CONFIG_PARAVIRT_SPINLOCKS
#undef CONFIG_TRACEPOINTS
#undef CONFIG_TRACE_IRQFLAGS
#undef CONFIG_DEBUG_IRQFLAGS
/* FIXME: Disable SGX to simplify POC */
#undef CONFIG_X86_SGX_KVM
#undef CONFIG_PREEMPT_COUNT
#define __NO_FORTIFY

unsigned long pkvm_virt_to_phys(void *virt);
#undef __pa
#define __pa(x) pkvm_virt_to_phys((void *)(x))

#endif /* __PKVM_X86_DEF_H */
