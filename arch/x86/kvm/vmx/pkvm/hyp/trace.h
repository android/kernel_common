/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _HYP_TRACE_H_
#define _HYP_TRACE_H_

void trace_vmexit_start(struct kvm_vcpu *vcpu);
void trace_vmexit_end(struct kvm_vcpu *vcpu, u32 index);
void pkvm_handle_set_vmexit_trace(bool en);
void pkvm_handle_dump_vmexit_trace(unsigned long pa, unsigned long size);
void pkvm_vcpu_perf_init(struct kvm_vcpu *vcpu);

#endif
