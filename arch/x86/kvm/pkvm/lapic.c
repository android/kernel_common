// SPDX-License-Identifier: GPL-2.0-only
#include <lapic.h>

static inline bool kvm_lapic_lvt_supported(struct kvm_lapic *apic, int lvt_index)
{
	return apic->nr_lvt_entries > lvt_index;
}

#define APIC_REG_MASK(reg)	(1ull << ((reg) >> 4))
#define APIC_REGS_MASK(first, count) \
	(APIC_REG_MASK(first) * ((1ull << (count)) - 1))

u64 kvm_lapic_readable_reg_mask(struct kvm_lapic *apic)
{
	/* Leave bits '0' for reserved and write-only registers. */
	u64 valid_reg_mask =
		APIC_REG_MASK(APIC_ID) |
		APIC_REG_MASK(APIC_LVR) |
		APIC_REG_MASK(APIC_TASKPRI) |
		APIC_REG_MASK(APIC_PROCPRI) |
		APIC_REG_MASK(APIC_LDR) |
		APIC_REG_MASK(APIC_SPIV) |
		APIC_REGS_MASK(APIC_ISR, APIC_ISR_NR) |
		APIC_REGS_MASK(APIC_TMR, APIC_ISR_NR) |
		APIC_REGS_MASK(APIC_IRR, APIC_ISR_NR) |
		APIC_REG_MASK(APIC_ESR) |
		APIC_REG_MASK(APIC_ICR) |
		APIC_REG_MASK(APIC_LVTT) |
		APIC_REG_MASK(APIC_LVTTHMR) |
		APIC_REG_MASK(APIC_LVTPC) |
		APIC_REG_MASK(APIC_LVT0) |
		APIC_REG_MASK(APIC_LVT1) |
		APIC_REG_MASK(APIC_LVTERR) |
		APIC_REG_MASK(APIC_TMICT) |
		APIC_REG_MASK(APIC_TMCCT) |
		APIC_REG_MASK(APIC_TDCR);

	if (kvm_lapic_lvt_supported(apic, LVT_CMCI))
		valid_reg_mask |= APIC_REG_MASK(APIC_LVTCMCI);

	/* ARBPRI, DFR, and ICR2 are not valid in x2APIC mode. */
	if (!apic_x2apic_mode(apic))
		valid_reg_mask |= APIC_REG_MASK(APIC_ARBPRI) |
				  APIC_REG_MASK(APIC_DFR) |
				  APIC_REG_MASK(APIC_ICR2);

	return valid_reg_mask;
}
EXPORT_SYMBOL_GPL(kvm_lapic_readable_reg_mask);
