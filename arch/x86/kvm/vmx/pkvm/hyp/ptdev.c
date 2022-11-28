// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022 Intel Corporation. */

#include <linux/hashtable.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>
#include "pkvm_hyp.h"
#include "iommu.h"
#include "ptdev.h"
#include "bug.h"

#define MAX_PTDEV_NUM	(PKVM_MAX_PDEV_NUM + PKVM_MAX_PASID_PDEV_NUM)
static DEFINE_HASHTABLE(ptdev_hasht, 8);
static DECLARE_BITMAP(ptdevs_bitmap, MAX_PTDEV_NUM);
static struct pkvm_ptdev pkvm_ptdev[MAX_PTDEV_NUM];
static pkvm_spinlock_t ptdev_lock = __PKVM_SPINLOCK_UNLOCKED;

struct pkvm_ptdev *pkvm_get_ptdev(u16 bdf, u32 pasid)
{
	struct pkvm_ptdev *ptdev = NULL, *tmp;
	unsigned long index;

	pkvm_spin_lock(&ptdev_lock);

	hash_for_each_possible(ptdev_hasht, tmp, hnode, bdf) {
		if (match_ptdev(tmp, bdf, pasid)) {
			ptdev = atomic_inc_not_zero(&tmp->refcount) ? tmp : NULL;
			if (ptdev)
				break;
		}
	}

	if (ptdev)
		goto out;

	index = find_next_zero_bit(ptdevs_bitmap, MAX_PTDEV_NUM, 0);
	if (index < MAX_PTDEV_NUM) {
		__set_bit(index, ptdevs_bitmap);
		ptdev = &pkvm_ptdev[index];
		ptdev->bdf = bdf;
		ptdev->pasid = pasid;
		ptdev->index = index;
		ptdev->pgt = pkvm_hyp->host_vm.ept;
		INIT_LIST_HEAD(&ptdev->iommu_node);
		INIT_LIST_HEAD(&ptdev->vm_node);
		atomic_set(&ptdev->refcount, 1);
		hash_add(ptdev_hasht, &ptdev->hnode, bdf);
	}
out:
	pkvm_spin_unlock(&ptdev_lock);

	return ptdev;
}

void pkvm_put_ptdev(struct pkvm_ptdev *ptdev)
{
	if (!atomic_dec_and_test(&ptdev->refcount))
		return;

	pkvm_spin_lock(&ptdev_lock);

	hlist_del(&ptdev->hnode);

	__clear_bit(ptdev->index, ptdevs_bitmap);

	memset(ptdev, 0, sizeof(struct pkvm_ptdev));

	pkvm_spin_unlock(&ptdev_lock);
}

void pkvm_setup_ptdev_vpgt(struct pkvm_ptdev *ptdev, unsigned long root_gpa,
			   struct pkvm_mm_ops *mm_ops, struct pkvm_pgtable_ops *paging_ops,
			   struct pkvm_pgtable_cap *cap)
{
	if (!root_gpa || root_gpa == INVALID_ADDR || !mm_ops || !paging_ops || !cap) {
		memset(&ptdev->vpgt, 0, sizeof(struct pkvm_pgtable));
		return;
	}

	ptdev->vpgt.root_pa = root_gpa;
	PKVM_ASSERT(pkvm_pgtable_init(&ptdev->vpgt, mm_ops, paging_ops, cap, false) == 0);
}

void pkvm_setup_ptdev_did(struct pkvm_ptdev *ptdev, u16 did)
{
	ptdev->did = did;
}

/*
 * pkvm_detach_ptdev()	- detach a ptdev from the shadow VM it is attached.
 * Basically it reverts what pkvm_attach_ptdev() does.
 *
 * @ptdev:	The target ptdev.
 */
void pkvm_detach_ptdev(struct pkvm_ptdev *ptdev)
{
	/* Reset what the attach API has set */
	ptdev->shadow_vm_handle = 0;
	pkvm_put_ptdev(ptdev);
}

/*
 * pkvm_attach_ptdev() - attach a ptdev to a shadow VM so it will be isolated
 * from the primary VM.
 *
 * @bdf:	The bdf of this ptdev.
 * @pasid:	The pasid of this ptdev.
 * @vm:		The shadow VM which will be attached to.
 *
 * FIXME:
 * The passthrough devices attached to the protected VM is relying on KVM
 * high to send vmcall so that pKVM can know which device should be isolated.
 * But if KVM high has created a passthrough device for a protected VM without
 * using this vmcall to notify pKVM, pKVM should still be able to isolate this
 * passthrough device. To guarantee this, either needs pKVM to know the
 * passthrough devices information to isolate them independently or needs
 * protected VM to check with pKVM about its passthrough device info through
 * some vmcall. Currently neither way is available.
 */
struct pkvm_ptdev *pkvm_attach_ptdev(u16 bdf, u32 pasid, struct pkvm_shadow_vm *vm)
{
	struct pkvm_ptdev *ptdev = pkvm_get_ptdev(bdf, pasid);

	if (!ptdev)
		return NULL;

	if (cmpxchg(&ptdev->shadow_vm_handle, 0, vm->shadow_vm_handle) != 0) {
		pkvm_err("%s: ptdev with bdf 0x%x pasid 0x%x is already attached\n",
			 __func__, bdf, pasid);
		pkvm_put_ptdev(ptdev);
		return NULL;
	}

	return ptdev;
}
