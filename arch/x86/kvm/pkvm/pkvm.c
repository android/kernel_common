// SPDX-License-Identifier: GPL-2.0
#include <asm/processor.h>
#include <asm/kvm_pkvm.h>
#include "pkvm.h"

struct cpuinfo_x86 boot_cpu_data;
unsigned int tsc_khz;

/*
 * FIXME: This was defined in kvm/mmu/mmu.c but as this file is not used for
 * adding cpu state protection, there is no equivalent mmu.c in the pkvm
 * hypervisor, define the tdp_enabled here to simplify.
 */
bool tdp_enabled = true;
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);

unsigned long handle_kvm_call(unsigned long fn, unsigned long p1,
			      unsigned long p2, unsigned long p3)
{
	unsigned long ret = 0;

	switch (fn) {
	case __pkvm__enable_virtualization_cpu:
		ret = kvm_arch_enable_virtualization_cpu();
		break;
	case __pkvm__disable_virtualization_cpu:
		kvm_arch_disable_virtualization_cpu();
		break;
	case __pkvm__check_processor_compatibility:
		ret = kvm_x86_call(check_processor_compatibility)();
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
