/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_TRACE_H_
#define _PKVM_TRACE_H_

#include <asm/vmx.h>

struct vmexit_data {
	u64 total_count;
	u64 total_cycles;
	u64 reasons[MAX_EXIT_REASONS];
	u64 cycles[MAX_EXIT_REASONS];
};

struct perf_data {
	struct vmexit_data data;
	unsigned long long tsc;
};

struct vmexit_perf_dump {
	struct perf_data l1data;
	struct perf_data l2data;
	int cpu;
};

#define PKVM_HC_SET_VMEXIT_TRACE	0xabcd0001
#define PKVM_HC_DUMP_VMEXIT_TRACE	0xabcd0002

#endif
