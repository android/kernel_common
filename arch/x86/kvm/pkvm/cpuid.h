/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_CPUID_H
#define __PKVM_X86_CPUID_H

#include <cpuid.h>

int kvm_set_cpuid(struct kvm_vcpu *vcpu, struct kvm_cpuid_entry2 *e2, int nent);

#endif
