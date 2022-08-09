/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _HYP_TRACE_H_
#define _HYP_TRACE_H_

#include <pkvm_trace.h>

void trace_vmexit_start(struct kvm_vcpu *vcpu, bool nested_vmexit);
void trace_vmexit_end(struct kvm_vcpu *vcpu, u32 index);
void pkvm_handle_set_vmexit_trace(struct kvm_vcpu *vcpu, bool en);
void pkvm_handle_dump_vmexit_trace(unsigned long pa, unsigned long size);

#endif
