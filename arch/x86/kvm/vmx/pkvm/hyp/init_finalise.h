/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_INIT_FINALISE_H
#define __PKVM_X86_INIT_FINALISE_H

int __pkvm_init_finalise(struct kvm_vcpu *vcpu, struct pkvm_section sections[],
			 int section_sz);
#endif
