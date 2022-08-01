/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#ifndef _PKVM_LAPIC_H_
#define _PKVM_LAPIC_H_

int pkvm_setup_lapic(struct pkvm_pcpu *pcpu, int cpu);
void pkvm_apic_base_msr_write(struct kvm_vcpu *vcpu, u64 apicbase);
int pkvm_x2apic_msr_write(struct kvm_vcpu *vcpu, u32 msr, u64 val);
void pkvm_lapic_send_init(struct pkvm_pcpu *dst_pcpu);
#endif
