// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (C) 2018-2022 Intel Corporation
 */

#include <pkvm.h>
#include "cpu.h"
#include "nested.h"
#include "lapic.h"
#include "debug.h"
#include "vmsr.h"

#define INTERCEPT_DISABLE		(0U)
#define INTERCEPT_READ			(1U << 0U)
#define INTERCEPT_WRITE			(1U << 1U)
#define INTERCEPT_READ_WRITE		(INTERCEPT_READ | INTERCEPT_WRITE)

static unsigned int emulated_ro_guest_msrs[] = {
	LIST_OF_VMX_MSRS,
};

static unsigned int emulated_wo_guest_msrs[] = {
	MSR_IA32_APICBASE,
	(APIC_BASE_MSR + (APIC_ID >> 4)),
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
	unsigned long msr = vcpu->arch.regs[VCPU_REGS_RCX];
	int ret = 0;
	u32 low = 0, high = 0;
	u64 val;

	/* For non-supported MSRs, return low=high=0 by default */
	if (is_vmx_msr(msr)) {
		ret = read_vmx_msr(vcpu, msr, &val);
		if (!ret) {
			low = (u32)val;
			high = (u32)(val >> 32);
		}
	}
	pkvm_dbg("%s: CPU%d Value of msr 0x%lx: low=0x%x, high=0x%x\n", __func__, vcpu->cpu, msr, low, high);

	vcpu->arch.regs[VCPU_REGS_RAX] = low;
	vcpu->arch.regs[VCPU_REGS_RDX] = high;

	return ret;
}

int handle_write_msr(struct kvm_vcpu *vcpu)
{
	unsigned long msr = vcpu->arch.regs[VCPU_REGS_RCX];
	u32 low, high;
	u64 val;
	int ret = 0;

	low = vcpu->arch.regs[VCPU_REGS_RAX];
	high = vcpu->arch.regs[VCPU_REGS_RDX];
	val = low | ((u64)high << 32);

	switch (msr) {
	case MSR_IA32_APICBASE:
		pkvm_apic_base_msr_write(vcpu, val);
		break;
	case APIC_BASE_MSR ... APIC_BASE_MSR + 0xff:
		ret = pkvm_x2apic_msr_write(vcpu, msr, val);
		break;
	default:
		break;
	}

	return ret;
}

void init_msr_emulation(struct vcpu_vmx *vmx)
{
	int i;
	u8 *bitmap = (u8 *)vmx->loaded_vmcs->msr_bitmap;

	for (i = 0; i < ARRAY_SIZE(emulated_ro_guest_msrs); i++)
		enable_msr_interception(bitmap, emulated_ro_guest_msrs[i], INTERCEPT_READ);

	for (i = 0; i < ARRAY_SIZE(emulated_wo_guest_msrs); i++)
		enable_msr_interception(bitmap, emulated_wo_guest_msrs[i], INTERCEPT_WRITE);
}
