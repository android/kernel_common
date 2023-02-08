/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright(c) 2022 Intel Corporation.
 * Copyright(c) 2023 Semihalf.
 */

#include <linux/hashtable.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>
#include "pkvm_hyp.h"
#include "gfp.h"
#include "iommu_spgt.h"
#include "ept.h"
#include "bug.h"

static DEFINE_HASHTABLE(iommu_spgt_hasht, 8);
static DECLARE_BITMAP(iommu_spgt_bitmap, PKVM_MAX_PDEV_NUM);
static struct pkvm_iommu_spgt pkvm_iommu_spgt[PKVM_MAX_PDEV_NUM];
static pkvm_spinlock_t iommu_spgt_lock = __PKVM_SPINLOCK_UNLOCKED;

struct pkvm_pgtable *pkvm_get_host_iommu_spgt(unsigned long root_gpa, bool coherency)
{
	struct pkvm_iommu_spgt *spgt = NULL, *tmp;
	unsigned long index;
	int ret;

	pkvm_spin_lock(&iommu_spgt_lock);

	hash_for_each_possible(iommu_spgt_hasht, tmp, hnode, root_gpa) {
		if (tmp->root_gpa == root_gpa) {
			if (tmp->refcount > 0) {
				spgt = tmp;
				break;
			}
		}
	}

	if (spgt) {
		spgt->refcount++;
		spgt->noncoherent_count += !coherency;
		pkvm_shadow_sl_iommu_pgt_update_coherency(&spgt->pgt,
							  !spgt->noncoherent_count);
		goto out;
	}

	index = find_first_zero_bit(iommu_spgt_bitmap, PKVM_MAX_PDEV_NUM);
	if (index < PKVM_MAX_PDEV_NUM) {
		spgt = &pkvm_iommu_spgt[index];

		ret = pkvm_pgtable_init(&spgt->pgt,
					pkvm_shadow_sl_iommu_pgt_get_mm_ops(coherency),
					&ept_ops, &pkvm_hyp->ept_cap, true);
		if (ret) {
			pkvm_err("%s: pgtable init failed err=%d\n", __func__, ret);
			spgt = NULL;
			goto out;
		}

		__set_bit(index, iommu_spgt_bitmap);
		spgt->root_gpa = root_gpa;
		spgt->index = index;
		spgt->refcount = 1;
		spgt->noncoherent_count = !coherency;
		hash_add(iommu_spgt_hasht, &spgt->hnode, root_gpa);
	}
out:
	pkvm_spin_unlock(&iommu_spgt_lock);

	return spgt ? &spgt->pgt : NULL;
}

void pkvm_put_host_iommu_spgt(struct pkvm_pgtable *pgt, bool coherency)
{
	struct pkvm_iommu_spgt *spgt = NULL, *tmp;
	int bkt;

	pkvm_spin_lock(&iommu_spgt_lock);

	hash_for_each(iommu_spgt_hasht, bkt, tmp, hnode) {
		if (&tmp->pgt == pgt) {
			spgt = tmp;
			break;
		}
	}
	PKVM_ASSERT(spgt);
	PKVM_ASSERT(spgt->refcount > 0);

	if (--spgt->refcount > 0) {
		spgt->noncoherent_count -= !coherency;
		PKVM_ASSERT(spgt->noncoherent_count >= 0);
		pkvm_shadow_sl_iommu_pgt_update_coherency(&spgt->pgt,
							  !spgt->noncoherent_count);
		goto out;
	}

	hash_del(&spgt->hnode);

	__clear_bit(spgt->index, iommu_spgt_bitmap);

	pkvm_pgtable_destroy(&spgt->pgt, NULL);

	memset(spgt, 0, sizeof(struct pkvm_iommu_spgt));

out:
	pkvm_spin_unlock(&iommu_spgt_lock);
}
