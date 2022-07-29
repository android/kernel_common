/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#include "pkvm.h"
#include "cpu.h"
#include "memory.h"
#include "mmu.h"
#include "pgtable.h"
#include "bug.h"

struct pkvm_lapic {
	bool x2apic;
	u32 apic_id;
	unsigned long apic_base_phys;
	void *apic_base_va;
};

static struct pkvm_lapic pkvm_lapic[CONFIG_NR_CPUS];

#define APIC_BASE_PHYS_MASK GENMASK_ULL(get_max_physaddr_bits(), 12)

static u32 __pkvm_lapic_read(struct pkvm_lapic *lapic, u32 reg)
{
	u64 val;

	if (lapic->x2apic)
		pkvm_rdmsrl(APIC_BASE_MSR + (reg >> 4), val);
	else
		val = readl(lapic->apic_base_va + reg);

	return (u32)val;
}

static int __pkvm_setup_lapic(struct pkvm_lapic *lapic, u64 apicbase)
{
	/* Not allow lapic to be disabled as it will be used for kick */
	PKVM_ASSERT(apicbase & (X2APIC_ENABLE | XAPIC_ENABLE));

	if (!(apicbase & X2APIC_ENABLE)) {
		unsigned long base_phys = apicbase & APIC_BASE_PHYS_MASK;
		void *vaddr = pkvm_iophys_to_virt(base_phys);

		if ((unsigned long)vaddr == INVALID_ADDR)
			return -EINVAL;

		if ((lapic->apic_base_phys == base_phys) &&
				(lapic->apic_base_va == vaddr))
			goto done;

		/* unmap the previous MMIO mapping then map the new one */
		if (lapic->apic_base_va) {
			pkvm_mmu_unmap((unsigned long)lapic->apic_base_va,
					PAGE_SIZE);
			lapic->apic_base_phys = 0;
			lapic->apic_base_va = NULL;
		}

		if (pkvm_mmu_map((unsigned long)vaddr, base_phys, PAGE_SIZE,
				 0, PKVM_PAGE_IO_NOCACHE))
			return -ENOMEM;

		lapic->apic_base_phys = base_phys;
		lapic->apic_base_va = vaddr;
		lapic->x2apic = false;
	} else
		lapic->x2apic = true;
done:
	/*
	 * APIC_ID reg is writable for primary VM so it is
	 * possible for primary VM to change the APIC_ID.
	 * So pkvm should have a way to intercept the APIC_ID
	 * changing. For x2apic mode, this can be done through
	 * intercepting the APIC_ID msr write.
	 *
	 * TODO: handling the APIC_ID changing for xapic mode.
	 */
	lapic->apic_id = __pkvm_lapic_read(lapic, APIC_ID);

	return 0;
}

static inline bool is_lapic_setup(struct pkvm_pcpu *pcpu)
{
	return !!pcpu->lapic;
}

int pkvm_setup_lapic(struct pkvm_pcpu *pcpu, int cpu)
{
	struct pkvm_lapic *lapic = &pkvm_lapic[cpu];
	u64 apicbase;

	/* Nothing needs to be done if already setup */
	if (is_lapic_setup(pcpu))
		return 0;

	pkvm_rdmsrl(MSR_IA32_APICBASE, apicbase);

	pcpu->lapic = lapic;

	return __pkvm_setup_lapic(lapic, apicbase);
}

void pkvm_apic_base_msr_write(struct kvm_vcpu *vcpu, u64 apicbase)
{
	struct pkvm_pcpu *pcpu = to_pkvm_hvcpu(vcpu)->pcpu;

	/*
	 * MSR is accessed before the init finalizing phase
	 * so pkvm not setup lapic yet. In this case, let the
	 * wrmsr directly go to the hardware.
	 */
	if (!is_lapic_setup(pcpu)) {
		pkvm_wrmsrl(MSR_IA32_APICBASE, apicbase);
		return;
	}

	/* A fatal error when is running at runtime */
	PKVM_ASSERT(__pkvm_setup_lapic(pcpu->lapic, apicbase) == 0);

	pkvm_wrmsrl(MSR_IA32_APICBASE, apicbase);
}

int pkvm_x2apic_msr_write(struct kvm_vcpu *vcpu, u32 msr, u64 val)
{
	struct pkvm_pcpu *pcpu = to_pkvm_hvcpu(vcpu)->pcpu;
	struct pkvm_lapic *lapic = pcpu->lapic;
	u32 reg = (msr - APIC_BASE_MSR) << 4;

	/*
	 * MSR is accessed before the init finalizing phase
	 * so pkvm not setup lapic yet. In this case, let the
	 * wrmsr directly go to the hardware.
	 */
	if (!is_lapic_setup(pcpu)) {
		pkvm_wrmsrl(msr, val);
		return 0;
	}

	/* Ensure lapic is in x2apic mode */
	if (!lapic->x2apic)
		return -EINVAL;

	switch (reg) {
	case APIC_ID:
		/*
		 * Not allow primary VM to modify the lapic ID which
		 * can result in pkvm failed to kick.
		 */
		PKVM_ASSERT(lapic->apic_id == (u32)val);
		break;
	default:
		break;
	}

	pkvm_wrmsrl(msr, val);
	return 0;
}
