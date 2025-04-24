/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#ifndef __ARM64_KVM_NVHE_PVIOMMU_H__
#define __ARM64_KVM_NVHE_PVIOMMU_H__

bool kvm_handle_pviommu_hvc(struct kvm_vcpu *vcpu, u64 *exit_code);

#endif /* __ARM64_KVM_NVHE_PVIOMMU_H__ */
