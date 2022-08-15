/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef __PKVM_IOMMU_INTERNAL_H
#define __PKVM_IOMMU_INTERNAL_H

#include <../drivers/iommu/intel/iommu.h>
#include <asm/pkvm.h>
#include <asm/pkvm_spinlock.h>
#include "pgtable.h"

#define PKVM_QI_DESC_ALIGNED_SIZE		ALIGN(QI_LENGTH * sizeof(struct qi_desc), PAGE_SIZE)
#define PKVM_QI_DESC_STATUS_ALIGNED_SIZE	ALIGN(QI_LENGTH * sizeof(int), PAGE_SIZE)

struct viommu_reg {
	u64 cap;
	u64 ecap;
	u32 gsts;
	u64 rta;
	u64 iq_head;
	u64 iq_tail;
	u64 iqa;
};

struct pkvm_viommu {
	struct pkvm_pgtable pgt;
	struct viommu_reg vreg;
	u64 iqa;
};

struct pkvm_iommu {
	struct intel_iommu iommu;
	pkvm_spinlock_t lock;
	bool activated;
	struct pkvm_pgtable pgt;
	struct pkvm_viommu viommu;

	struct q_inval qi;
	pkvm_spinlock_t qi_lock;
	u64 piommu_iqa;
};

enum lm_level {
	IOMMU_LM_CONTEXT = 1,
	IOMMU_LM_ROOT,
};

enum sm_level {
	IOMMU_PASID_TABLE = 1,
	IOMMU_PASID_DIR,
	IOMMU_SM_CONTEXT,
	IOMMU_SM_ROOT,
	IOMMU_SM_LEVEL_NUM,
};


#define LAST_LEVEL(level)	\
	((level == 1) ? true : false)

#define LM_DEVFN_BITS	8
#define LM_DEVFN_SHIFT	0

#define LM_BUS_BITS		8
#define LM_BUS_SHIFT	8

#define PASID_PTE_PRESENT	1
#define PASID_PTE_FPD		2
#define MAX_NR_PASID_BITS	PKVM_MAX_PASID_BITS

#define PASIDTAB_BITS		6
#define PASIDTAB_SHIFT		0

#define PASIDDIR_BITS		(MAX_NR_PASID_BITS - PASIDTAB_BITS)
#define PASIDDIR_SHIFT		PASIDTAB_BITS

#define DEVFN_BITS		8
#define DEVFN_SHIFT		(PASIDDIR_SHIFT + PASIDDIR_BITS)

#define BUS_BITS		8
#define BUS_SHIFT		(DEVFN_SHIFT + DEVFN_BITS)

/* Used to calculate the level-to-index */
#define SM_DEVFN_BITS		7
#define SM_BUS_BITS		9
#define SM_BUS_SHIFT		(DEVFN_SHIFT + SM_DEVFN_BITS)

#define IOMMU_MAX_VADDR_LEN	(BUS_SHIFT + BUS_BITS)
#define IOMMU_MAX_VADDR		BIT(IOMMU_MAX_VADDR_LEN)

#define DMAR_GSTS_EN_BITS	(DMA_GCMD_TE | DMA_GCMD_EAFL | \
				 DMA_GCMD_QIE | DMA_GCMD_IRE | \
				 DMA_GCMD_CFI)
#define DMAR_GCMD_PROTECTED	(DMA_GCMD_TE | DMA_GCMD_SRTP | \
				 DMA_GCMD_QIE)
#define DMAR_GCMD_DIRECT	(DMA_GCMD_SFL | DMA_GCMD_EAFL | \
				 DMA_GCMD_WBF | DMA_GCMD_IRE | \
				 DMA_GCMD_SIRTP | DMA_GCMD_CFI)

#define PKVM_IOMMU_WAIT_OP(offset, op, cond, sts)			\
do {									\
	while (1) {							\
		sts = op(offset);					\
		if (cond)						\
			break;						\
		cpu_relax();						\
	}								\
} while (0)

#define IQ_DESC_BASE_PHYS(reg)		(reg & ~0xfff)
#define IQ_DESC_DW(reg)			((reg >> 11) & 1)
#define IQ_DESC_QS(reg)			(reg & GENMASK_ULL(2, 0))
#define IQ_DESC_LEN(reg)		(1 << (7 + IQ_DESC_QS(reg) + !IQ_DESC_DW(reg)))
#define IQ_DESC_SHIFT(reg)		(4 + IQ_DESC_DW(reg))

#define QI_DESC_TYPE(qw)		(qw & GENMASK_ULL(3, 0))
#define QI_DESC_CC_GRANU(qw)		((qw & GENMASK_ULL(5, 4)) >> 4)
#define QI_DESC_CC_DID(qw)		((qw & GENMASK_ULL(31, 16)) >> 16)
#define QI_DESC_CC_SID(qw)		((qw & GENMASK_ULL(47, 32)) >> 32)

#define QI_DESC_PC_GRANU(qw)		((qw & GENMASK_ULL(5, 4)) >> 4)
#define QI_DESC_PC_DID(qw)		((qw & GENMASK_ULL(31, 16)) >> 16)
#define QI_DESC_PC_PASID(qw)		((qw & GENMASK_ULL(51, 32)) >> 32)

struct pasid_dir_entry {
	u64 val;
};

struct pasid_entry {
	u64 val[8];
};

static inline void entry_set_bits(u64 *ptr, u64 mask, u64 bits)
{
	u64 old;

	old = READ_ONCE(*ptr);
	WRITE_ONCE(*ptr, (old & ~mask) | bits);
}

static inline void context_clear_dte(struct context_entry *ce)
{
	entry_set_bits(&ce->lo, 1 << 2, 0);
}

/* Get PRESENT bit of a PASID table entry. */
static inline bool pasid_pte_is_present(struct pasid_entry *pte)
{
	return READ_ONCE(pte->val[0]) & PASID_PTE_PRESENT;
}

/* Get PGTT field of a PASID table entry */
static inline u16 pasid_pte_get_pgtt(struct pasid_entry *pte)
{
	return (u16)((READ_ONCE(pte->val[0]) >> 6) & 0x7);
}

/*
 * Interfaces for PASID table entry manipulation:
 */
static inline void pasid_clear_entry(struct pasid_entry *pe)
{
	WRITE_ONCE(pe->val[0], 0);
	WRITE_ONCE(pe->val[1], 0);
	WRITE_ONCE(pe->val[2], 0);
	WRITE_ONCE(pe->val[3], 0);
	WRITE_ONCE(pe->val[4], 0);
	WRITE_ONCE(pe->val[5], 0);
	WRITE_ONCE(pe->val[6], 0);
	WRITE_ONCE(pe->val[7], 0);
}

/*
 * Get domain ID value of a scalable mode PASID entry.
 */
static inline u16
pasid_get_domain_id(struct pasid_entry *pe)
{
	return (u16)(READ_ONCE(pe->val[1]) & GENMASK_ULL(15, 0));
}

/*
 * Setup the SLPTPTR(Second Level Page Table Pointer) field (Bit 12~63)
 * of a scalable mode PASID entry.
 */
static inline void
pasid_set_slptr(struct pasid_entry *pe, u64 value)
{
	entry_set_bits(&pe->val[0], VTD_PAGE_MASK, value);
}

/*
 * Setup the AW(Address Width) field (Bit 2~4) of a scalable mode PASID
 * entry.
 */
static inline void
pasid_set_address_width(struct pasid_entry *pe, u64 value)
{
	entry_set_bits(&pe->val[0], GENMASK_ULL(4, 2), value << 2);
}

/*
 * Setup the PGTT(PASID Granular Translation Type) field (Bit 6~8)
 * of a scalable mode PASID entry.
 */
static inline void
pasid_set_translation_type(struct pasid_entry *pe, u64 value)
{
	entry_set_bits(&pe->val[0], GENMASK_ULL(8, 6), value << 6);
}

/*
 * Setup Page Walk Snoop bit (Bit 87) of a scalable mode PASID
 * entry.
 */
static inline void pasid_set_page_snoop(struct pasid_entry *pe, bool value)
{
	entry_set_bits(&pe->val[1], 1 << 23, value << 23);
}

/*
 * Setup the Page Snoop (PGSNP) field (Bit 88) of a scalable mode
 * PASID entry.
 */
static inline void
pasid_set_pgsnp(struct pasid_entry *pe)
{
	entry_set_bits(&pe->val[1], 1ULL << 24, 1ULL << 24);
}

#define PASID_ENTRY_PGTT_FL_ONLY        (1)
#define PASID_ENTRY_PGTT_SL_ONLY        (2)
#define PASID_ENTRY_PGTT_NESTED         (3)
#define PASID_ENTRY_PGTT_PT             (4)

/*
 * Set the Second Stage Execute Enable field (Bit 5) of a scalable mode
 * PASID entry.
 */
static inline void pasid_set_ssee(struct pasid_entry *pe, bool value)
{
	entry_set_bits(&pe->val[0], 1 << 5, value << 5);
}

/*
 * Set the Second Stage Access/Dirty bit Enable field (Bit 9) of a scalable mode
 * PASID entry.
 */
static inline void pasid_set_ssade(struct pasid_entry *pe, bool value)
{
	entry_set_bits(&pe->val[0], 1 << 9, value << 9);
}

static inline bool pasid_copy_entry(struct pasid_entry *to, struct pasid_entry *from)
{
	bool updated = false;
	int i;

	for (i = 0; i < 8; i++) {
		u64 new = READ_ONCE(from->val[i]);

		if (READ_ONCE(to->val[i]) != new) {
			WRITE_ONCE(to->val[i], new);
			updated = true;
		}
	}

	return updated;
}

extern void root_tbl_walk(struct pkvm_iommu *iommu);

#endif
