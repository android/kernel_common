/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_POWER_DOMAIN_H
#define __KVM_POWER_DOMAIN_H

#include <linux/android_kabi.h>

enum kvm_power_domain_type {
	KVM_POWER_DOMAIN_NONE,
	KVM_POWER_DOMAIN_HOST_HVC,
	KVM_POWER_DOMAIN_ARM_SCMI,
};

struct kvm_power_domain {
	enum kvm_power_domain_type	type;
	union {
		u64 device_id; /* HOST_HVC device ID*/
		struct {
			u32		smc_id;
			u32		domain_id;
			phys_addr_t	shmem_base;
			size_t		shmem_size;
		} arm_scmi; /*ARM_SCMI channel */
	};
	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
};

#endif /* __KVM_POWER_DOMAIN_H */
