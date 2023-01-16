/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2022 Intel Corporation. */

#ifndef _PKVM_PTDEV_H_
#define _PKVM_PTDEV_H_

#include "pgtable.h"

struct pkvm_ptdev {
	atomic_t refcount;
	struct hlist_node hnode;
	u16 did;
	u16 bdf;
	u32 pasid;
	unsigned long index;
	struct list_head iommu_node;
	bool iommu_coherency;

	/* Represents the page table maintained by primary VM */
	struct pkvm_pgtable vpgt;
	/* Represents the page table maintained by pKVM */
	struct pkvm_pgtable *pgt;

	int shadow_vm_handle;
	struct list_head vm_node;
};

struct pkvm_ptdev *pkvm_alloc_ptdev(u16 bdf, u32 pasid, bool coherency);
struct pkvm_ptdev *pkvm_get_ptdev(u16 bdf, u32 pasid);
void pkvm_put_ptdev(struct pkvm_ptdev *ptdev);
void pkvm_setup_ptdev_vpgt(struct pkvm_ptdev *ptdev, unsigned long root_gpa,
			   struct pkvm_mm_ops *mm_ops, struct pkvm_pgtable_ops *paging_ops,
			   struct pkvm_pgtable_cap *cap);
void pkvm_setup_ptdev_did(struct pkvm_ptdev *ptdev, u16 did);
void pkvm_detach_ptdev(struct pkvm_ptdev *ptdev, struct pkvm_shadow_vm *vm);
int pkvm_attach_ptdev(u16 bdf, u32 pasid, struct pkvm_shadow_vm *vm);

static inline bool match_ptdev(struct pkvm_ptdev *ptdev, u16 bdf, u32 pasid)
{
	return ptdev && (ptdev->bdf == bdf) && (ptdev->pasid == pasid);
}

static inline bool ptdev_attached_to_vm(struct pkvm_ptdev *ptdev)
{
	/* Attached ptdev has non-zero shadow_vm_handle */
	return cmpxchg(&ptdev->shadow_vm_handle, 0, 0) != 0;
}
#endif
