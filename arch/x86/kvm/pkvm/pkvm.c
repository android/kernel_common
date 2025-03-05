// SPDX-License-Identifier: GPL-2.0
#include <asm/processor.h>
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
	return -EINVAL;
}
