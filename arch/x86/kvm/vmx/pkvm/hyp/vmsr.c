// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (C) 2018-2022 Intel Corporation
 */

#include <pkvm.h>
#include "cpu.h"
#include "debug.h"

#define INTERCEPT_DISABLE		(0U)
#define INTERCEPT_READ			(1U << 0U)
#define INTERCEPT_WRITE			(1U << 1U)
#define INTERCEPT_READ_WRITE		(INTERCEPT_READ | INTERCEPT_WRITE)

static unsigned int emulated_ro_guest_msrs[] = {
	/* DUMMY */
};

static void enable_msr_interception(u8 *bitmap, unsigned int msr_arg, unsigned int mode)
{
	unsigned int read_offset = 0U;
	unsigned int write_offset = 2048U;
	unsigned int msr = msr_arg;
	u8 msr_bit;
	unsigned int msr_index;

	if ((msr <= 0x1FFFU) || ((msr >= 0xc0000000U) && (msr <= 0xc0001fffU))) {
		if ((msr & 0xc0000000U) != 0U) {
			read_offset = read_offset + 1024U;
			write_offset = write_offset + 1024U;
		}

		msr &= 0x1FFFU;
		msr_bit = (u8)(1U << (msr & 0x7U));
		msr_index = msr >> 3U;

		if ((mode & INTERCEPT_READ) == INTERCEPT_READ)
			bitmap[read_offset + msr_index] |= msr_bit;
		else
			bitmap[read_offset + msr_index] &= ~msr_bit;

		if ((mode & INTERCEPT_WRITE) == INTERCEPT_WRITE)
			bitmap[write_offset + msr_index] |= msr_bit;
		else
			bitmap[write_offset + msr_index] &= ~msr_bit;
	} else {
		pkvm_err("%s, Invalid MSR: 0x%x", __func__, msr);
	}
}

int handle_read_msr(struct kvm_vcpu *vcpu)
{
	/* simply return 0 for non-supported MSRs */
	vcpu->arch.regs[VCPU_REGS_RAX] = 0;
	vcpu->arch.regs[VCPU_REGS_RDX] = 0;

	return 0;
}

int handle_write_msr(struct kvm_vcpu *vcpu)
{
	/*No emulation for msr write now*/
	return 0;
}

void init_msr_emulation(struct vcpu_vmx *vmx)
{
	int i;
	u8 *bitmap = (u8 *)vmx->loaded_vmcs->msr_bitmap;

	for (i = 0; i < ARRAY_SIZE(emulated_ro_guest_msrs); i++)
		enable_msr_interception(bitmap, emulated_ro_guest_msrs[i], INTERCEPT_READ);
}
