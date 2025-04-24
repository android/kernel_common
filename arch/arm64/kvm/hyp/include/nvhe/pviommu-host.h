// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#ifndef __PVIOMMU_HOST_H
#define __PVIOMMU_HOST_H

#include <linux/kvm_host.h>
#include <nvhe/pkvm.h>

/* Ideally these are dynamically allocated. */
#define MAX_NR_PVIOMMU					32
#define MAX_NR_SID_PER_PVIOMMU				16

int pkvm_pviommu_attach(struct kvm *host_kvm, int pviommu);
int pkvm_pviommu_add_vsid(struct kvm *host_kvm, int pviommu,
			  pkvm_handle_t iommu, u32 sid, u32 vsid);
int pkvm_pviommu_finalise(struct pkvm_hyp_vm *hyp_vm);
void pkvm_pviommu_teardown(struct pkvm_hyp_vm *hyp_vm);

/**
 * struct pviommu_entry - A single entry (endpoint) in a pvIOMMU
 * @iommu:		physical IOMMU ID as defined by the pKVM IOMMU module.
 * @sid:		Physical endpoint ID
 * @vsid:		Virtual endpoint ID
 */
struct pviommu_entry {
	pkvm_handle_t iommu;
	u32 sid;
	u32 vsid;
};

/**
 * struct pviommu_host - pvIOMMU created by host
 * @kvm:		VM which pvIOMMU is attached to.
 * @pviommu_id:	ID of the pvIOMMU which is seen by guest.
 * @nr_entries:	Number of struct pviommu_entry in the pvIOMMU.
 * @entries:	Entries connected to pvIOMMU (Endpoints)
 * @list:		list used to connected pvIOMMU in the same VM.
 * @finalized:	Mark that pvIOMMU can't be changed by host anymore.
 */
struct pviommu_host {
	struct kvm *kvm;
	int pviommu_id;
	int nr_entries;
	struct pviommu_entry entries[MAX_NR_SID_PER_PVIOMMU];
	struct list_head list;
	bool finalized;
};

#endif /* __PVIOMMU_HOST_H */
