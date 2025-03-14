/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_H
#define __PKVM_X86_H

#include <x86.h>

void kvm_vcpu_enter_guest(struct kvm_vcpu *vcpu, bool force_immediate_exit);

#endif /* __PKVM_X86_H */
