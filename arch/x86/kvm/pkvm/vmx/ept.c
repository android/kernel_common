// SPDX-License-Identifier: GPL-2.0
#include <asm/kvm_pkvm.h>
#include <mmu.h>
#include <mmu/spte.h>

#include <pkvm/mmu.h>
#include "ept.h"
//FIXME: clean up the header files
#include <vmx/pkvm/hyp/pgtable.h>
#include <pkvm.h>

static bool ept_entry_present(void *ptep)
{
	u64 val = *(u64 *)ptep;

	return !!(val & VMX_EPT_RWX_MASK);
}

static bool ept_entry_mapped(void *ptep)
{
	/*
	 * Both present and non-zero non-present entries are counted as
	 * mapped because a non-present entry with a non-zero value may
	 * contain page state and ownership information created through
	 * a map operation. So simply count non-zero entry as mapped to
	 * cover both cases.
	 */
	return !!(*(u64 *)ptep);
}

static bool ept_entry_huge(void *ptep)
{
	return is_large_pte(*(u64 *)ptep);
}

static void ept_entry_mkhuge(void *ptep)
{
	*(u64 *)ptep |= PT_PAGE_SIZE_MASK;
}

static unsigned long ept_entry_to_phys(void *ptep)
{
	return *(u64 *)ptep & SPTE_BASE_ADDR_MASK;
}

static u64 ept_entry_to_prot(void *ptep)
{
	u64 prot = *(u64 *)ptep & ~(SPTE_BASE_ADDR_MASK);

	return prot & ~PT_PAGE_SIZE_MASK;
}

static u64 ept_entry_calc_perm(bool read, bool write, bool exec)
{
	u64 prot = 0;

	if (read)
		prot |= VMX_EPT_READABLE_MASK;
	if (write)
		prot |= VMX_EPT_WRITABLE_MASK;
	if (exec)
		prot |= VMX_EPT_EXECUTABLE_MASK;

	return prot;
}

static int ept_entry_to_index(unsigned long vaddr, int level)
{
	return SPTE_INDEX(vaddr, level);
}

static bool ept_entry_is_leaf(void *ptep, int level)
{
	return level == PG_LEVEL_4K ||
	       !ept_entry_present(ptep) ||
	       ept_entry_huge(ptep);
}

static int ept_level_entry_size(int level)
{
	return PAGE_SIZE / SPTE_ENT_PER_PAGE;
}

static int ept_level_to_entries(int level)
{
	return SPTE_ENT_PER_PAGE;
}

static u64 ept_level_page_mask(int level)
{
	return ~((1UL << SPTE_LEVEL_SHIFT(level)) - 1);
}

static unsigned long ept_level_to_size(int level)
{
	return KVM_HPAGE_SIZE(level);
}

static bool ept_entry_young(void *ptep)
{
	u64 val = *(u64 *)ptep;

	return !!(val & VMX_EPT_ACCESS_BIT);
}

static void ept_entry_mkold(void *ptep)
{
	*(u64 *)ptep &= ~VMX_EPT_ACCESS_BIT;
}

static void ept_set_entry(void *sptep, u64 spte)
{
	WRITE_ONCE(*(u64 *)sptep, spte);
}

static const struct pkvm_pgtable_ops ept_pgt_ops = {
	.pgt_entry_present = ept_entry_present,
	.pgt_entry_mapped = ept_entry_mapped,
	.pgt_entry_huge = ept_entry_huge,
	.pgt_entry_mkhuge = ept_entry_mkhuge,
	.pgt_entry_to_phys = ept_entry_to_phys,
	.pgt_entry_to_prot = ept_entry_to_prot,
	.pgt_entry_calc_perm = ept_entry_calc_perm,
	.pgt_entry_to_index = ept_entry_to_index,
	.pgt_level_page_mask = ept_level_page_mask,
	.pgt_entry_is_leaf = ept_entry_is_leaf,
	.pgt_level_entry_size = ept_level_entry_size,
	.pgt_level_to_entries = ept_level_to_entries,
	.pgt_level_to_size = ept_level_to_size,
	.pgt_entry_young = ept_entry_young,
	.pgt_entry_mkold = ept_entry_mkold,
	.pgt_set_entry = ept_set_entry,
};

void pkvm_guest_ept_setup(void)
{
	guest_mmu_pgt_ops = &ept_pgt_ops;
	guest_mmu_pgt_cap = (struct pkvm_pgtable_cap) {
		.level = pkvm_hyp->vmx_cap.ept & VMX_EPT_PAGE_WALK_5_BIT ? 5 : 4,
		.allowed_pgsz = 1 << PG_LEVEL_4K,
		.table_prot = VMX_EPT_RWX_MASK,
	};

	if (pkvm_hyp->vmx_cap.ept & VMX_EPT_2MB_PAGE_BIT)
		guest_mmu_pgt_cap.allowed_pgsz |= 1 << PG_LEVEL_2M;
	if (pkvm_hyp->vmx_cap.ept & VMX_EPT_1GB_PAGE_BIT)
		guest_mmu_pgt_cap.allowed_pgsz |= 1 << PG_LEVEL_1G;
}
