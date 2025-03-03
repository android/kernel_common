/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_PKVM_H
#define __PKVM_X86_PKVM_H

unsigned long handle_kvm_call(unsigned long fn, unsigned long p1,
			      unsigned long p2, unsigned long p3);

#endif /* __PKVM_X86_PKVM_H */
