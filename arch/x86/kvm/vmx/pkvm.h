/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_VMX_PKVM_H
#define __KVM_X86_VMX_PKVM_H

static unsigned long pkvm_hypercall(unsigned long nr, unsigned long p1,
				    unsigned long p2, unsigned long p3,
				    unsigned long p4)
{
	unsigned long ret;

	asm volatile("vmcall"
		     : "=a"(ret)
		     : "a"(nr), "b"(p1), "c"(p2), "d"(p3), "S"(p4)
		     : "memory");
	return ret;
}

#define CALL_PKVM(f)		CONCATENATE(__pkvm__, f)

#define __kvm_call_pkvm_0(f)	pkvm_hypercall(PKVM_HC_KVM_CALL, f, 0, 0, 0)

#define __kvm_call_pkvm_1(f, a1)							\
	({										\
		pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), 0, 0);					\
	})

#define __kvm_call_pkvm_2(f, a1, a2)							\
	({										\
		pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), (unsigned long)(a2), 0);			\
	})

#define __kvm_call_pkvm_3(f, a1, a2, a3)						\
	({										\
		pkvm_hypercall(PKVM_HC_KVM_CALL, f,					\
			(unsigned long)(a1), (unsigned long)(a2),			\
			(unsigned long)(a3));						\
	})

#define kvm_call_pkvm(f, ...)								\
	({										\
		CONCATENATE(__kvm_call_pkvm_,						\
			    COUNT_ARGS(__VA_ARGS__))(CALL_PKVM(f), ##__VA_ARGS__);	\
	})

#endif /* __KVM_X86_VMX_PKVM_H */
