// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <nvhe/pviommu.h>
#include <nvhe/pviommu-host.h>

struct pviommu_host pviommus[MAX_NR_PVIOMMU];
static DEFINE_HYP_SPINLOCK(host_pviommu_lock);

/*
 * Attach a new pvIOMMU instance to VM host_kvm, and assign
 * pviommu as an ID to it.
 */
int pkvm_pviommu_attach(struct kvm *host_kvm, int pviommu)
{
	int i, ret = -EBUSY;

	if (!host_kvm)
		return -EINVAL;
	hyp_spin_lock(&host_pviommu_lock);
	for (i = 0 ; i < MAX_NR_PVIOMMU ; ++i) {
		struct pviommu_host *ph = &pviommus[i];

		if (!ph->kvm && !ph->finalized) {
			ph->kvm = kern_hyp_va(host_kvm);
			ph->pviommu_id = pviommu;
			ret = 0;
			break;
		}
	}
	hyp_spin_unlock(&host_pviommu_lock);
	return ret;
}

/*
 * Although, having 1:many vsid:psid relation might have valid use cases,
 * that complicates the hypervisor interface when dealing with attach/detach
 * hypercalls.
 * So, for now we constraint that a vsid only maps to a one psid.
 * The other way around is allowed (many:1 vsid:psid). However that
 * might not have as common cases, as that means changes to one of the
 * vsids would reflect to the others as they have the same psid.
 */
static int __pkvm_pviommu_vsid_valid(struct pviommu_host *ph, u32 vsid)
{
	int i;

	for (i = 0 ; i < ph->nr_entries ; ++i) {
		if (ph->entries[i].vsid == vsid)
			return -EEXIST;
	}
	return 0;
}

/*
 * For a pvIOMMU with ID pviommu, that is attached to host_kvm
 * add new entry for a virtual sid, that maps to a physical IOMMU
 * with id iommu and sid.
 */
int pkvm_pviommu_add_vsid(struct kvm *host_kvm, int pviommu,
			  pkvm_handle_t iommu, u32 sid, u32 vsid)
{
	int i;
	int ret = -ENOENT;

	hyp_spin_lock(&host_pviommu_lock);
	for (i = 0 ; i < MAX_NR_PVIOMMU ; ++i) {
		struct pviommu_host *ph = &pviommus[i];

		if (!ph->kvm || ph->finalized)
			continue;
		if (ph->pviommu_id == pviommu && ph->kvm == kern_hyp_va(host_kvm)) {
			if (ph->nr_entries == MAX_NR_SID_PER_PVIOMMU) {
				ret = -EBUSY;
				break;
			}
			ret = __pkvm_pviommu_vsid_valid(ph, vsid);
			if (ret)
				break;

			ph->entries[ph->nr_entries].sid = sid;
			ph->entries[ph->nr_entries].vsid = vsid;
			ph->entries[ph->nr_entries].iommu = iommu;
			ph->nr_entries++;
			break;
		}
	}
	hyp_spin_unlock(&host_pviommu_lock);
	return ret;
}

/*
 * Called at vm init, adds all the pvIOMMUs belonging to the VM
 * in a list. No more changes allowed from the host to any of
 * those pvIOMMU
 */
int pkvm_pviommu_finalise(struct pkvm_hyp_vm *hyp_vm)
{
	int i;
	int ret;

	ret = hyp_pool_init_empty(&hyp_vm->iommu_pool, 64);
	if (ret)
		return ret;

	hyp_spin_lock(&host_pviommu_lock);
	INIT_LIST_HEAD(&hyp_vm->pviommus);
	INIT_LIST_HEAD(&hyp_vm->domains);
	for (i = 0; i < MAX_NR_PVIOMMU ; ++i) {
		struct pviommu_host *ph = &pviommus[i];

		if (ph->kvm == hyp_vm->host_kvm) {
			ph->finalized = true;
			list_add_tail(&ph->list, &hyp_vm->pviommus);
		}
	}
	hyp_spin_unlock(&host_pviommu_lock);
	return 0;
}

/*
 * Called when VM is torndown, to free pvIOMMU instance and clean
 * any state.
 */
void pkvm_pviommu_teardown(struct pkvm_hyp_vm *hyp_vm)
{
	struct pviommu_host *ph;

	hyp_spin_lock(&host_pviommu_lock);
	list_for_each_entry(ph, &hyp_vm->pviommus, list) {
		/* pvIOMMU is free now. */
		ph->kvm = NULL;
		ph->nr_entries = 0;
		ph->finalized = false;
	}
	kvm_iommu_teardown_guest_domains(hyp_vm);
	hyp_spin_unlock(&host_pviommu_lock);
}

int pkvm_pviommu_get_route(struct pkvm_hyp_vm *hyp_vm, pkvm_handle_t pviommu, u32 vsid,
			   struct pviommu_route *route)
{
	struct pviommu_host *ph;
	int i;

	list_for_each_entry(ph, &hyp_vm->pviommus, list) {
		if (ph->pviommu_id == pviommu) {
			for (i = 0 ; i < ph->nr_entries ; ++i) {
				if (ph->entries[i].vsid == vsid) {
					route->sid = ph->entries[i].sid;
					route->iommu = ph->entries[i].iommu;
					return 0;
				}
			}
			break;
		}
	}
	return -ENOENT;
}
