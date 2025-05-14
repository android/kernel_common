// SPDX-License-Identifier: GPL-2.0-only
#include <asm/fpu/api.h>
#include <asm/fpu/types.h>
#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>
#include <asm/msr.h>

#include "internal.h"
#include "xstate.h"

#ifdef CONFIG_X86_64
DEFINE_STATIC_KEY_FALSE(__fpu_state_size_dynamic);
DEFINE_PER_CPU(u64, xfd_state);
#endif

/* The FPU state configuration data for kernel and user space */
struct fpu_state_config fpu_kernel_cfg __ro_after_init;
struct fpu_state_config fpu_user_cfg __ro_after_init;

static inline void fpstate_init_fxstate(struct fpstate *fpstate)
{
	fpstate->regs.fxsave.cwd = 0x37f;
	fpstate->regs.fxsave.mxcsr = MXCSR_DEFAULT;
}

/*
 * Legacy x87 fpstate state init:
 */
static inline void fpstate_init_fstate(struct fpstate *fpstate)
{
	fpstate->regs.fsave.cwd = 0xffff037fu;
	fpstate->regs.fsave.swd = 0xffff0000u;
	fpstate->regs.fsave.twd = 0xffffffffu;
	fpstate->regs.fsave.fos = 0xffff0000u;
}

/*
 * Used in two places:
 * 1) Early boot to setup init_fpstate for non XSAVE systems
 * 2) fpu_init_fpstate_user() which is invoked from KVM
 */
void fpstate_init_user(struct fpstate *fpstate)
{
	if (!cpu_feature_enabled(X86_FEATURE_FPU)) {
#ifndef __PKVM_HYP__
		fpstate_init_soft(&fpstate->regs.soft);
#endif
		return;
	}

	xstate_init_xcomp_bv(&fpstate->regs.xsave, fpstate->xfeatures);

	if (cpu_feature_enabled(X86_FEATURE_FXSR))
		fpstate_init_fxstate(fpstate);
	else
		fpstate_init_fstate(fpstate);
}

/*
 * fpu_enable_guest_xfd_features - Check xfeatures against guest perm and enable
 * @guest_fpu:         Pointer to the guest FPU container
 * @xfeatures:         Features requested by guest CPUID
 *
 * Enable all dynamic xfeatures according to guest perm and requested CPUID.
 *
 * Return: 0 on success, error code otherwise
 */
int fpu_enable_guest_xfd_features(struct fpu_guest *guest_fpu, u64 xfeatures)
{
#ifndef __PKVM_HYP__
	lockdep_assert_preemption_enabled();
#endif

	/* Nothing to do if all requested features are already enabled. */
	xfeatures &= ~guest_fpu->xfeatures;
	if (!xfeatures)
		return 0;

	return __xfd_enable_feature(xfeatures, guest_fpu);
}
EXPORT_SYMBOL_GPL(fpu_enable_guest_xfd_features);
