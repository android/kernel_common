/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 - Google LLC
 * Author: Andrew Walbran <qwandor@google.com>
 */
#ifndef __KVM_HYP_FFA_H
#define __KVM_HYP_FFA_H

#include <asm/kvm_host.h>
#include <nvhe/pkvm.h>

#define FFA_MIN_FUNC_NUM 0x60
#define FFA_MAX_FUNC_NUM 0xFF

/*
 * "ID value 0 must be returned at the Non-secure physical FF-A instance"
 * We share this ID with the host.
 */
#define HOST_FFA_ID	0

struct ffa_mem_transfer {
	struct list_head node;
	u64 ffa_handle;
	struct list_head translations;
};

int hyp_ffa_init(void *pages);
bool kvm_host_ffa_handler(struct kvm_cpu_context *host_ctxt, u32 func_id);
bool kvm_guest_ffa_handler(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code);
struct ffa_mem_transfer *find_transfer_by_handle(u64 ffa_handle, struct kvm_ffa_buffers *buf);
int kvm_dying_guest_reclaim_ffa_resources(struct pkvm_hyp_vm *vm);

#endif /* __KVM_HYP_FFA_H */
