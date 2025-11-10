/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_TRACE_H_
#define _PKVM_TRACE_H_

#include <asm/pkvm_spinlock.h>
#include <asm/pkvm.h>
#include <asm/vmx.h>

struct vmexit_stats {
	u64 count;
	u64 cycles;
};

struct vmexit_data {
	struct vmexit_stats reasons[MAX_EXIT_REASONS];
	struct vmexit_stats hypercalls[MAX_PKVM_HYPERCALLS];
	struct vmexit_stats total;
};

struct perf_data {
	struct vmexit_data vmexit;
	int vm_handle;
	int vcpu_id;
};

struct vmexit_perf {
	pkvm_spinlock_t lock;
	struct perf_data data;
	unsigned long long tsc;
	unsigned long rax;
	unsigned int age;
};

#endif
