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
#endif

#endif /* __PKVM_X86_DEF_H */
