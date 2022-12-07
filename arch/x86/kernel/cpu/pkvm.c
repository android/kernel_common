// SPDX-License-Identifier: GPL-2.0
/*
 * PKVM detection support.
 */

#include <asm/hypervisor.h>
#include <asm/pkvm.h>

static u32 __init pkvm_detect(void)
{
	if (boot_cpu_has(X86_FEATURE_HYPERVISOR))
		return hypervisor_cpuid_base("PKVMPKVMPKVM", 0);

	return 0;
}

static void __init pkvm_init_platform(void)
{
	pkvm_guest_init_coco();
}

static bool pkvm_x2apic_available(void)
{
	return boot_cpu_has(X86_FEATURE_X2APIC);
}

const __initconst struct hypervisor_x86 x86_hyper_pkvm = {
	.name                   = "PKVM",
	.detect                 = pkvm_detect,
	.type			= X86_HYPER_PKVM,
	.init.init_platform     = pkvm_init_platform,
	.init.x2apic_available  = pkvm_x2apic_available,
};
