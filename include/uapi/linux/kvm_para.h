/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI__LINUX_KVM_PARA_H
#define _UAPI__LINUX_KVM_PARA_H

#include <linux/const.h>
#include <linux/types.h>

/*
 * This header file provides a method for making a hypercall to the host
 * Architectures should define:
 * - kvm_hypercall0, kvm_hypercall1...
 * - kvm_arch_para_features
 * - kvm_para_available
 */

/* Return values for hypercalls */
#define KVM_ENOSYS		1000
#define KVM_EFAULT		EFAULT
#define KVM_EINVAL		EINVAL
#define KVM_E2BIG		E2BIG
#define KVM_EPERM		EPERM
#define KVM_EOPNOTSUPP		95

#define KVM_HC_VAPIC_POLL_IRQ		1
#define KVM_HC_MMU_OP			2
#define KVM_HC_FEATURES			3
#define KVM_HC_PPC_MAP_MAGIC_PAGE	4
#define KVM_HC_KICK_CPU			5
#define KVM_HC_MIPS_GET_CLOCK_FREQ	6
#define KVM_HC_MIPS_EXIT_VM		7
#define KVM_HC_MIPS_CONSOLE_OUTPUT	8
#define KVM_HC_CLOCK_PAIRING		9
#define KVM_HC_SEND_IPI		10
#define KVM_HC_SCHED_YIELD		11
#define KVM_HC_MAP_GPA_RANGE		12

#define KVM_HC_PKVM_OP			20

/* PKVM provided hypercalls for guest use. */
#define PKVM_GHC_NUM(x)	(x + KVM_HC_PKVM_OP)

#define PKVM_GHC_SHARE_MEM		PKVM_GHC_NUM(1)
#define PKVM_GHC_UNSHARE_MEM		PKVM_GHC_NUM(2)
#define PKVM_GHC_IOREAD			PKVM_GHC_NUM(3)
#define PKVM_GHC_IOWRITE		PKVM_GHC_NUM(4)

/*
 * PKVM_GHC_GET_VE_INFO is deprecated, it is not used by pKVM guests
 * anymore. Keep it defined just for the case when compiling as a
 * host kernel with pKVM hypervisor enabled, not as a guest kernel.
 */
#define PKVM_GHC_GET_VE_INFO		PKVM_GHC_NUM(5)

enum kerncs_boost_type {
	PVSCHED_KERNCS_BOOST_PREEMPT_DISABLED =	0x1,
	PVSCHED_KERNCS_BOOST_IRQ =		0x2,
	PVSCHED_KERNCS_BOOST_SOFTIRQ =		0x4,
	PVSCHED_KERNCS_BOOST_IDLE =		0x8,
	PVSCHED_KERNCS_BOOST_ALL =		0xF,
};

union vcpu_sched_attr {
	struct {
		__u8	enabled;
		__u8	sched_policy;
		__s8	sched_nice;
		__u8	rt_priority;
		/*
		 * Guest running a kernel critical section:
		 * - nmi, irq, softirq, preemption disabled.
		 */
		__u8	kern_cs;
	};
	__u64	pad;
};

enum pv_schedattr_type {
	PV_SCHEDATTR_GUEST = 0,
	PV_SCHEDATTR_HOST,
	PV_SCHEDATTR_MAX
};

/*
 * Offset of guest area in the PV_SCHED shared memory.
 */
#define PV_SCHEDATTR_GUEST_OFFSET	0

/*
 * Offset of the host area in the PV_SCHED shared memory.
 */
#define PV_SCHEDATTR_HOST_OFFSET	(sizeof(union vcpu_sched_attr))

/*
 * PARAVIRT_SCHED info shared between host and guest.
 */
struct pv_sched_data {
	union vcpu_sched_attr attr[PV_SCHEDATTR_MAX];
};

/*
 * hypercalls use architecture specific
 */
#include <asm/kvm_para.h>

#endif /* _UAPI__LINUX_KVM_PARA_H */
