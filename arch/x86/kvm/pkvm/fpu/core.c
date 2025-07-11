// SPDX-License-Identifier: GPL-2.0-only
#include <asm/fpu/api.h>
#include <asm/fpu/types.h>
#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>

#include "internal.h"
#include "xstate.h"

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
