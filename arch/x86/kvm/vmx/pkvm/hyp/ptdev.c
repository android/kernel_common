// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022 Intel Corporation. */

#include <linux/hashtable.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>
#include "pkvm_hyp.h"
#include "iommu.h"
#include "ptdev.h"
#include "iommu_spgt.h"
#include "bug.h"

#define MAX_PTDEV_NUM	(PKVM_MAX_PDEV_NUM + PKVM_MAX_PASID_PDEV_NUM)
static DEFINE_HASHTABLE(ptdev_hasht, 8);
static DECLARE_BITMAP(ptdevs_bitmap, MAX_PTDEV_NUM);
static struct pkvm_ptdev pkvm_ptdev[MAX_PTDEV_NUM];
static pkvm_spinlock_t ptdev_lock = __PKVM_SPINLOCK_UNLOCKED;

struct pkvm_ptdev *pkvm_alloc_ptdev(u16 bdf, u32 pasid, bool coherency)
{
	struct pkvm_ptdev *ptdev = NULL;
	unsigned long index;

	pkvm_spin_lock(&ptdev_lock);

	index = find_next_zero_bit(ptdevs_bitmap, MAX_PTDEV_NUM, 0);
	if (index < MAX_PTDEV_NUM) {
		__set_bit(index, ptdevs_bitmap);
		ptdev = &pkvm_ptdev[index];
		ptdev->bdf = bdf;
		ptdev->pasid = pasid;
		ptdev->iommu_coherency = coherency;
		ptdev->index = index;
		ptdev->pgt = pkvm_hyp->host_vm.ept;
		INIT_LIST_HEAD(&ptdev->iommu_node);
		INIT_LIST_HEAD(&ptdev->vm_node);
		atomic_set(&ptdev->refcount, 1);
		pkvm_spin_lock_init(&ptdev->lock);
		hash_add(ptdev_hasht, &ptdev->hnode, bdf);
	}

	pkvm_spin_unlock(&ptdev_lock);

	return ptdev;
}

struct pkvm_ptdev *pkvm_get_ptdev(u16 bdf, u32 pasid)
{
	struct pkvm_ptdev *ptdev = NULL, *tmp;

	pkvm_spin_lock(&ptdev_lock);

	hash_for_each_possible(ptdev_hasht, tmp, hnode, bdf) {
		if (match_ptdev(tmp, bdf, pasid)) {
			ptdev = atomic_inc_not_zero(&tmp->refcount) ? tmp : NULL;
			if (ptdev)
				break;
		}
	}

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

	if (ptdev->pgt != pkvm_hyp->host_vm.ept)
		pkvm_put_host_iommu_spgt(ptdev->pgt, ptdev->iommu_coherency);

	memset(ptdev, 0, sizeof(struct pkvm_ptdev));

	pkvm_spin_unlock(&ptdev_lock);
}

void pkvm_setup_ptdev_vpgt(struct pkvm_ptdev *ptdev, unsigned long root_gpa,
			   struct pkvm_mm_ops *mm_ops, struct pkvm_pgtable_ops *paging_ops,
			   struct pkvm_pgtable_cap *cap, bool shadowed)
{
	pkvm_spin_lock(&ptdev->lock);

	if (ptdev->pgt != pkvm_hyp->host_vm.ept &&
			(!shadowed || root_gpa != ptdev->vpgt.root_pa) &&
			!ptdev_attached_to_vm(ptdev)) {
		pkvm_put_host_iommu_spgt(ptdev->pgt, ptdev->iommu_coherency);
		ptdev->pgt = pkvm_hyp->host_vm.ept;
	}

	if (!root_gpa || root_gpa == INVALID_ADDR || !mm_ops || !paging_ops || !cap) {
		memset(&ptdev->vpgt, 0, sizeof(struct pkvm_pgtable));
		goto out;
	}

	ptdev->vpgt.root_pa = root_gpa;
	PKVM_ASSERT(pkvm_pgtable_init(&ptdev->vpgt, mm_ops, paging_ops, cap, false) == 0);

	if (shadowed && ptdev->pgt == pkvm_hyp->host_vm.ept) {
		ptdev->pgt = pkvm_get_host_iommu_spgt(root_gpa, ptdev->iommu_coherency);
		PKVM_ASSERT(ptdev->pgt);
	}
out:
	pkvm_spin_unlock(&ptdev->lock);
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
 * @vm:		The shadow VM which will be attached to.
 */
void pkvm_detach_ptdev(struct pkvm_ptdev *ptdev, struct pkvm_shadow_vm *vm)
{
	/* Reset what the attach API has set */
	pkvm_spin_lock(&ptdev->lock);
	ptdev->shadow_vm_handle = 0;
	ptdev->pgt = pkvm_hyp->host_vm.ept;
	pkvm_spin_unlock(&ptdev->lock);

	pkvm_shadow_vm_unlink_ptdev(vm, &ptdev->vm_node,
				    ptdev->iommu_coherency);
	pkvm_iommu_sync(ptdev->bdf, ptdev->pasid);

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
int pkvm_attach_ptdev(u16 bdf, u32 pasid, struct pkvm_shadow_vm *vm)
{
	struct pkvm_ptdev *ptdev = pkvm_get_ptdev(bdf, pasid);

	if (!ptdev) {
		ptdev = pkvm_alloc_ptdev(bdf, pasid,
					 pkvm_iommu_coherency(bdf, pasid));
		if (!ptdev)
			return -ENODEV;
	}

	pkvm_spin_lock(&ptdev->lock);

	if (cmpxchg(&ptdev->shadow_vm_handle, 0, vm->shadow_vm_handle) != 0) {
		pkvm_err("%s: ptdev with bdf 0x%x pasid 0x%x is already attached\n",
			 __func__, bdf, pasid);
		pkvm_spin_unlock(&ptdev->lock);
		pkvm_put_ptdev(ptdev);
		return -ENODEV;
	}

	PKVM_ASSERT(ptdev->pgt != &vm->pgstate_pgt);
	if (ptdev->pgt != pkvm_hyp->host_vm.ept)
		pkvm_put_host_iommu_spgt(ptdev->pgt, ptdev->iommu_coherency);

	/*
	 * Reset pgt of this ptdev to VM's pgstate_pgt so need to update
	 * IOMMU page table accordingly.
	 */
	ptdev->pgt = &vm->pgstate_pgt;

	pkvm_spin_unlock(&ptdev->lock);

	pkvm_shadow_vm_link_ptdev(vm, &ptdev->vm_node,
				  ptdev->iommu_coherency);
	if (pkvm_iommu_sync(ptdev->bdf, ptdev->pasid)) {
		pkvm_detach_ptdev(ptdev, vm);
		return -ENODEV;
	}

	return 0;
}
