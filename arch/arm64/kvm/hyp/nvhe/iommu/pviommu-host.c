// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <nvhe/pviommu-host.h>

struct pviommu_host pviommus[MAX_NR_PVIOMMU];

/*
 * Attach a new pvIOMMU instance to VM host_kvm, and assign
 * pviommu as an ID to it.
 */
int pkvm_pviommu_attach(struct kvm *host_kvm, int pviommu)
{
	return -ENODEV;
}

/*
 * For a pvIOMMU with ID pviommu, that is attached to host_kvm
 * add new entry for a virtual sid, that maps to a physical IOMMU
 * with id iommu and sid.
 */
int pkvm_pviommu_add_vsid(struct kvm *host_kvm, int pviommu,
			  pkvm_handle_t iommu, u32 sid, u32 vsid)
{
	return -ENODEV;
}

/*
 * Called at vm init, adds all the pvIOMMUs belonging to the VM
 * in a list. No more changes allowed from the host to any of
 * those pvIOMMU
 */
int pkvm_pviommu_finalise(struct pkvm_hyp_vm *hyp_vm)
{
	return 0;
}

/*
 * Called when VM is torndown, to free pvIOMMU instance and clean
 * any state.
 */
void pkvm_pviommu_teardown(struct pkvm_hyp_vm *hyp_vm)
{
}
