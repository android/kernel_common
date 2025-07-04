// SPDX-License-Identifier: GPL-2.0-only
#include <asm/fpu/types.h>
#include <asm/fpu/xstate.h>

#include "internal.h"
#include "fpu.h"

#define for_each_extended_xfeature(bit, mask)				\
	(bit) = FIRST_EXTENDED_XFEATURE;				\
	for_each_set_bit_from(bit, (unsigned long *)&(mask), 8 * sizeof(mask))

static unsigned int xstate_offsets[XFEATURE_MAX] __ro_after_init = {
	[0 ... XFEATURE_MAX - 1] = -1
};
static unsigned int xstate_sizes[XFEATURE_MAX] __ro_after_init = {
	[0 ... XFEATURE_MAX - 1] = -1
};
static unsigned int xstate_flags[XFEATURE_MAX] __ro_after_init;

#define XSTATE_FLAG_SUPERVISOR	BIT(0)

static bool xfeature_is_supervisor(int xfeature_nr)
{
	return xstate_flags[xfeature_nr] & XSTATE_FLAG_SUPERVISOR;
}

/*
 * Record the offsets and sizes of various xstates contained
 * in the XSAVE state memory layout.
 */
static void __init setup_xstate_cache(void)
{
	u32 eax, ebx, ecx, edx, i;
	/* start at the beginning of the "extended state" */
	unsigned int last_good_offset = offsetof(struct xregs_state,
						 extended_state_area);
	/*
	 * The FP xstates and SSE xstates are legacy states. They are always
	 * in the fixed offsets in the xsave area in either compacted form
	 * or standard form.
	 */
	xstate_offsets[XFEATURE_FP]	= 0;
	xstate_sizes[XFEATURE_FP]	= offsetof(struct fxregs_state,
						   xmm_space);

	xstate_offsets[XFEATURE_SSE]	= xstate_sizes[XFEATURE_FP];
	xstate_sizes[XFEATURE_SSE]	= sizeof_field(struct fxregs_state,
						       xmm_space);

	for_each_extended_xfeature(i, fpu_kernel_cfg.max_features) {
		cpuid_count(XSTATE_CPUID, i, &eax, &ebx, &ecx, &edx);

		xstate_sizes[i] = eax;
		xstate_flags[i] = ecx;

		/*
		 * If an xfeature is supervisor state, the offset in EBX is
		 * invalid, leave it to -1.
		 */
		if (xfeature_is_supervisor(i))
			continue;

		xstate_offsets[i] = ebx;

		/*
		 * In our xstate size checks, we assume that the highest-numbered
		 * xstate feature has the highest offset in the buffer.  Ensure
		 * it does.
		 */
		WARN_ONCE(last_good_offset > xstate_offsets[i],
			  "x86/fpu: misordered xstate at %d\n", last_good_offset);

		last_good_offset = xstate_offsets[i];
	}
}

#ifdef __PKVM_HYP__
void pkvm_setup_xstate_cache(void)
{
	if (!boot_cpu_has(X86_FEATURE_FPU)) {
		pr_info("x86/fpu: No FPU detected\n");
		return;
	}

	if (!boot_cpu_has(X86_FEATURE_XSAVE)) {
		pr_info("x86/fpu: x87 FPU will use %s\n",
			boot_cpu_has(X86_FEATURE_FXSR) ? "FXSAVE" : "FSAVE");
		return;
	}

	if (boot_cpu_data.cpuid_level < XSTATE_CPUID) {
		WARN_ON_FPU(1);
		return;
	}

	setup_xstate_cache();
}
#endif
