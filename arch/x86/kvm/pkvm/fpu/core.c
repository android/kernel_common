// SPDX-License-Identifier: GPL-2.0-only
#include <asm/fpu/api.h>
#include <asm/fpu/sched.h>
#include <asm/fpu/signal.h>
#include <asm/fpu/types.h>
#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>
#include <asm/msr.h>

#include "internal.h"
#include "legacy.h"
#include "xstate.h"
#include "context.h"

#ifdef CONFIG_X86_64
DEFINE_STATIC_KEY_FALSE(__fpu_state_size_dynamic);
DEFINE_PER_CPU(u64, xfd_state);
#endif

/* The FPU state configuration data for kernel and user space */
struct fpu_state_config fpu_kernel_cfg __ro_after_init;
struct fpu_state_config fpu_user_cfg __ro_after_init;

/*
 * Save the FPU register state in fpu->fpstate->regs. The register state is
 * preserved.
 *
 * Must be called with fpregs_lock() held.
 *
 * The legacy FNSAVE instruction clears all FPU state unconditionally, so
 * register state has to be reloaded. That might be a pointless exercise
 * when the FPU is going to be used by another task right after that. But
 * this only affects 20+ years old 32bit systems and avoids conditionals all
 * over the place.
 *
 * FXSAVE and all XSAVE variants preserve the FPU register state.
 */
void save_fpregs_to_fpstate(struct fpu *fpu)
{
	if (likely(use_xsave())) {
		os_xsave(fpu->fpstate);
#ifndef __PKVM_HYP__
		update_avx_timestamp(fpu);
#endif
		return;
	}

	if (likely(use_fxsr())) {
		fxsave(&fpu->fpstate->regs.fxsave);
		return;
	}

	/*
	 * Legacy FPU register saving, FNSAVE always clears FPU registers,
	 * so we have to reload them from the memory state.
	 */
	asm volatile("fnsave %[fp]; fwait" : [fp] "=m" (fpu->fpstate->regs.fsave));
	frstor(&fpu->fpstate->regs.fsave);
}

void restore_fpregs_from_fpstate(struct fpstate *fpstate, u64 mask)
{
	/*
	 * AMD K7/K8 and later CPUs up to Zen don't save/restore
	 * FDP/FIP/FOP unless an exception is pending. Clear the x87 state
	 * here by setting it to fixed values.  "m" is a random variable
	 * that should be in L1.
	 */
	if (unlikely(static_cpu_has_bug(X86_BUG_FXSAVE_LEAK))) {
		asm volatile(
			"fnclex\n\t"
			"emms\n\t"
			"fildl %[addr]"	/* set F?P to defined value */
			: : [addr] "m" (*fpstate));
	}

	if (use_xsave()) {
		/*
		 * Dynamically enabled features are enabled in XCR0, but
		 * usage requires also that the corresponding bits in XFD
		 * are cleared.  If the bits are set then using a related
		 * instruction will raise #NM. This allows to do the
		 * allocation of the larger FPU buffer lazy from #NM or if
		 * the task has no permission to kill it which would happen
		 * via #UD if the feature is disabled in XCR0.
		 *
		 * XFD state is following the same life time rules as
		 * XSTATE and to restore state correctly XFD has to be
		 * updated before XRSTORS otherwise the component would
		 * stay in or go into init state even if the bits are set
		 * in fpstate::regs::xsave::xfeatures.
		 */
		xfd_update_state(fpstate);

		/*
		 * Restoring state always needs to modify all features
		 * which are in @mask even if the current task cannot use
		 * extended features.
		 *
		 * So fpstate->xfeatures cannot be used here, because then
		 * a feature for which the task has no permission but was
		 * used by the previous task would not go into init state.
		 */
		mask = fpu_kernel_cfg.max_features & mask;

		os_xrstor(fpstate, mask);
	} else {
		if (use_fxsr())
			fxrstor(&fpstate->regs.fxsave);
		else
			frstor(&fpstate->regs.fsave);
	}
}

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

#ifdef CONFIG_X86_64
void fpu_update_guest_xfd(struct fpu_guest *guest_fpu, u64 xfd)
{
#ifndef __PKVM_HYP__
	fpregs_lock();
#endif
	guest_fpu->fpstate->xfd = xfd;
	if (guest_fpu->fpstate->in_use)
		xfd_update_state(guest_fpu->fpstate);
#ifndef __PKVM_HYP__
	fpregs_unlock();
#endif
}
EXPORT_SYMBOL_GPL(fpu_update_guest_xfd);

/**
 * fpu_sync_guest_vmexit_xfd_state - Synchronize XFD MSR and software state
 *
 * Must be invoked from KVM after a VMEXIT before enabling interrupts when
 * XFD write emulation is disabled. This is required because the guest can
 * freely modify XFD and the state at VMEXIT is not guaranteed to be the
 * same as the state on VMENTER. So software state has to be updated before
 * any operation which depends on it can take place.
 *
 * Note: It can be invoked unconditionally even when write emulation is
 * enabled for the price of a then pointless MSR read.
 */
void fpu_sync_guest_vmexit_xfd_state(void)
{
	struct fpstate *fps = current->thread.fpu.fpstate;

#ifndef __PKVM_HYP__
	lockdep_assert_irqs_disabled();
#endif
	if (fpu_state_size_dynamic()) {
		rdmsrl(MSR_IA32_XFD, fps->xfd);
		__this_cpu_write(xfd_state, fps->xfd);
	}
}
EXPORT_SYMBOL_GPL(fpu_sync_guest_vmexit_xfd_state);
#endif

int fpu_swap_kvm_fpstate(struct fpu_guest *guest_fpu, bool enter_guest)
{
	struct fpstate *guest_fps = guest_fpu->fpstate;
	struct fpu *fpu = &current->thread.fpu;
	struct fpstate *cur_fps = fpu->fpstate;

#ifdef __PKVM_HYP__
#ifdef CONFIG_X86_64
	if (fpu_state_size_dynamic() && enter_guest) {
		/*
		 * Refresh the xfd_state percpu cache before guest vmenter so
		 * that the xfd can be restored after guest vmexit.
		 */
		rdmsrl(MSR_IA32_XFD, cur_fps->xfd);
		__this_cpu_write(xfd_state, cur_fps->xfd);
	}
#endif
	/*
	 * If entering the npVM, the FPU are already loaded with the npVM fpu
	 * state by the host. If exiting from the npVM, the fpu registers will be
	 * saved by the host. So no need to save FPU for the npVM.
	 *
	 * If entering the pVM, the FPU are loaded with the host fpu state, which
	 * is already saved by the host itself before switching to the pkvm
	 * hypervisor. If exiting from the pVM, then the fpu state should be saved
	 * by the pkvm hypervisor as the host is not allowed to do this for
	 * isolation purpose.
	 */
	if (guest_fps->is_confidential && !enter_guest)
		save_fpregs_to_fpstate(fpu);
#else
	fpregs_lock();
	if (!cur_fps->is_confidential && !test_thread_flag(TIF_NEED_FPU_LOAD))
		save_fpregs_to_fpstate(fpu);
#endif

	/* Swap fpstate */
	if (enter_guest) {
		fpu->__task_fpstate = cur_fps;
		fpu->fpstate = guest_fps;
		guest_fps->in_use = true;
	} else {
		guest_fps->in_use = false;
		fpu->fpstate = fpu->__task_fpstate;
		fpu->__task_fpstate = NULL;
	}

	cur_fps = fpu->fpstate;

#ifdef __PKVM_HYP__
	/*
	 * Similarly to the FPU saving case, no need to restore FPU for the npVM
	 * as this will be handled by the host.
	 *
	 * If entering the pVM, restore the FPU with the pVM fpu state. If
	 * exiting the pVM, wipe the FPU by restoring FPU with an initial fpu
	 * state.
	 */
	if (guest_fps->is_confidential) {
		/* Includes XFD update */
		restore_fpregs_from_fpstate(cur_fps, XFEATURE_MASK_FPSTATE);
	} else {
		/* Only update XFD as npVM FPU is already loaded by the host */
		xfd_update_state(cur_fps);
	}
#else
	if (!cur_fps->is_confidential) {
		/* Includes XFD update */
		restore_fpregs_from_fpstate(cur_fps, XFEATURE_MASK_FPSTATE);
	} else {
		/*
		 * XSTATE is restored by firmware from encrypted
		 * memory. Make sure XFD state is correct while
		 * running with guest fpstate
		 */
		xfd_update_state(cur_fps);
	}

	fpregs_mark_activate();
	fpregs_unlock();
#endif

	return 0;
}
EXPORT_SYMBOL_GPL(fpu_swap_kvm_fpstate);
