// SPDX-License-Identifier: GPL-2.0-only
#include <asm/fpu/types.h>
#include <asm/fpu/xstate.h>

#include "internal.h"
#include "xstate.h"
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
#define XSTATE_FLAG_ALIGNED64	BIT(1)

static bool xfeature_is_aligned64(int xfeature_nr)
{
	return xstate_flags[xfeature_nr] & XSTATE_FLAG_ALIGNED64;
}

static bool xfeature_is_supervisor(int xfeature_nr)
{
	return xstate_flags[xfeature_nr] & XSTATE_FLAG_SUPERVISOR;
}

static unsigned int xfeature_get_offset(u64 xcomp_bv, int xfeature)
{
	unsigned int offs, i;

	/*
	 * Non-compacted format and legacy features use the cached fixed
	 * offsets.
	 */
	if (!cpu_feature_enabled(X86_FEATURE_XCOMPACTED) ||
	    xfeature <= XFEATURE_SSE)
		return xstate_offsets[xfeature];

	/*
	 * Compacted format offsets depend on the actual content of the
	 * compacted xsave area which is determined by the xcomp_bv header
	 * field.
	 */
	offs = FXSAVE_SIZE + XSAVE_HDR_SIZE;
	for_each_extended_xfeature(i, xcomp_bv) {
		if (xfeature_is_aligned64(i))
			offs = ALIGN(offs, 64);
		if (i == xfeature)
			break;
		offs += xstate_sizes[i];
	}
	return offs;
}

static bool xfeature_enabled(enum xfeature xfeature)
{
	return fpu_kernel_cfg.max_features & BIT_ULL(xfeature);
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

static unsigned int xstate_calculate_size(u64 xfeatures, bool compacted)
{
	unsigned int topmost = fls64(xfeatures) -  1;
	unsigned int offset = xstate_offsets[topmost];

	if (topmost <= XFEATURE_SSE)
		return sizeof(struct xregs_state);

	if (compacted)
		offset = xfeature_get_offset(xfeatures, topmost);
	return offset + xstate_sizes[topmost];
}

/*
 * Given an xstate feature nr, calculate where in the xsave
 * buffer the state is.  Callers should ensure that the buffer
 * is valid.
 */
static void *__raw_xsave_addr(struct xregs_state *xsave, int xfeature_nr)
{
	u64 xcomp_bv = xsave->header.xcomp_bv;

	if (WARN_ON_ONCE(!xfeature_enabled(xfeature_nr)))
		return NULL;

	if (cpu_feature_enabled(X86_FEATURE_XCOMPACTED)) {
		if (WARN_ON_ONCE(!(xcomp_bv & BIT_ULL(xfeature_nr))))
			return NULL;
	}

	return (void *)xsave + xfeature_get_offset(xcomp_bv, xfeature_nr);
}

/*
 * Given the xsave area and a state inside, this function returns the
 * address of the state.
 *
 * This is the API that is called to get xstate address in either
 * standard format or compacted format of xsave area.
 *
 * Note that if there is no data for the field in the xsave buffer
 * this will return NULL.
 *
 * Inputs:
 *	xstate: the thread's storage area for all FPU data
 *	xfeature_nr: state which is defined in xsave.h (e.g. XFEATURE_FP,
 *	XFEATURE_SSE, etc...)
 * Output:
 *	address of the state in the xsave area, or NULL if the
 *	field is not present in the xsave buffer.
 */
void *get_xsave_addr(struct xregs_state *xsave, int xfeature_nr)
{
	/*
	 * Do we even *have* xsave state?
	 */
	if (!boot_cpu_has(X86_FEATURE_XSAVE))
		return NULL;

	/*
	 * We should not ever be requesting features that we
	 * have not enabled.
	 */
	if (WARN_ON_ONCE(!xfeature_enabled(xfeature_nr)))
		return NULL;

	/*
	 * This assumes the last 'xsave*' instruction to
	 * have requested that 'xfeature_nr' be saved.
	 * If it did not, we might be seeing and old value
	 * of the field in the buffer.
	 *
	 * This can happen because the last 'xsave' did not
	 * request that this feature be saved (unlikely)
	 * or because the "init optimization" caused it
	 * to not be saved.
	 */
	if (!(xsave->header.xfeatures & BIT_ULL(xfeature_nr)))
		return NULL;

	return __raw_xsave_addr(xsave, xfeature_nr);
}
EXPORT_SYMBOL_GPL(get_xsave_addr);

#if IS_ENABLED(CONFIG_KVM)
void fpstate_clear_xstate_component(struct fpstate *fps, unsigned int xfeature)
{
	void *addr = get_xsave_addr(&fps->regs.xsave, xfeature);

	if (addr)
		memset(addr, 0, xstate_sizes[xfeature]);
}
EXPORT_SYMBOL_GPL(fpstate_clear_xstate_component);
#endif

#ifdef CONFIG_X86_64
int __xfd_enable_feature(u64 xfd_err, struct fpu_guest *guest_fpu)
{
	u64 xfd_event = xfd_err & XFEATURE_MASK_USER_DYNAMIC;
#ifdef __PKVM_HYP__
	struct fpstate *fps;
	unsigned int ksize;

	if (!xfd_event)
		return 0;

	if (WARN_ON_ONCE(!guest_fpu))
		return -EINVAL;

	if ((xstate_get_group_perm(!!guest_fpu) & xfd_event) != xfd_event)
		return -EPERM;

	fps = guest_fpu->fpstate;
	ksize = xstate_calculate_size(fps->xfeatures | xfd_event,
				      cpu_feature_enabled(X86_FEATURE_XCOMPACTED));
	if (fps->size < ksize) {
		/* State size is insufficient. */
		return -ENOMEM;
	}

	guest_fpu->xfeatures |= xfd_event;
	fps->xfeatures |= xfd_event;
	fps->user_xfeatures |= xfd_event;
	fps->xfd &= ~xfd_event;

	xstate_init_xcomp_bv(&fps->regs.xsave, fps->xfeatures);
	if (fps->in_use)
		xfd_update_state(fps);

	return 0;
#else
	struct fpu_state_perm *perm;
	unsigned int ksize, usize;
	struct fpu *fpu;

	if (!xfd_event) {
		if (!guest_fpu)
			pr_err_once("XFD: Invalid xfd error: %016llx\n", xfd_err);
		return 0;
	}

	/* Protect against concurrent modifications */
	spin_lock_irq(&current->sighand->siglock);

	/* If not permitted let it die */
	if ((xstate_get_group_perm(!!guest_fpu) & xfd_event) != xfd_event) {
		spin_unlock_irq(&current->sighand->siglock);
		return -EPERM;
	}

	fpu = &current->group_leader->thread.fpu;
	perm = guest_fpu ? &fpu->guest_perm : &fpu->perm;
	ksize = perm->__state_size;
	usize = perm->__user_state_size;

	/*
	 * The feature is permitted. State size is sufficient.  Dropping
	 * the lock is safe here even if more features are added from
	 * another task, the retrieved buffer sizes are valid for the
	 * currently requested feature(s).
	 */
	spin_unlock_irq(&current->sighand->siglock);

	/*
	 * Try to allocate a new fpstate. If that fails there is no way
	 * out.
	 */
	if (fpstate_realloc(xfd_event, ksize, usize, guest_fpu))
		return -EFAULT;
	return 0;
#endif
}
#endif

u64 xstate_get_guest_group_perm(void)
{
	return xstate_get_group_perm(true);
}
EXPORT_SYMBOL_GPL(xstate_get_guest_group_perm);

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
