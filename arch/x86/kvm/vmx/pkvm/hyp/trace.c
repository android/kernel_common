/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/kvm_host.h>
#include <asm/pkvm_spinlock.h>
#include <asm/kvm_pkvm.h>
#include <pkvm.h>
#include "trace.h"
#include "debug.h"
#include "pkvm/pkvm.h"

/*
 * memset/memcpy can be re-defined by include/linux/fortify-string.h, which
 * may introduce additional linux kernel symbols. Undefine them to force
 * use the implementation in pkvm/hyp/lib/
 * */
#undef memset
#undef memcpy

struct perf_ctrl {
	unsigned int age;
	bool on;
};
static DEFINE_PER_CPU(struct vmexit_perf, hvcpu_perf);
static DEFINE_PER_CPU(struct perf_ctrl, perf_ctrl);

static inline unsigned long long pkvm_rdtsc_ordered(void)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("lfence;rdtsc" : EAX_EDX_RET(val, low, high));

	return EAX_EDX_VAL(val, low, high);
}

static inline bool is_host_vcpu(struct kvm_vcpu *vcpu)
{
	return this_cpu_read(host_vcpu) == vcpu;
}

static inline struct vmexit_perf *vcpu_to_perf(struct kvm_vcpu *vcpu)
{
	return is_host_vcpu(vcpu) ? this_cpu_ptr(&hvcpu_perf) :
				    &to_pkvm_vcpu(vcpu)->perf;
}

static void refresh_vmexit_perf(struct perf_ctrl *pctrl, struct vmexit_perf *perf)
{
	perf->age = pctrl->age;
	memset(&perf->data.vmexit, 0, sizeof(struct vmexit_data));
}

void trace_vmexit_start(struct kvm_vcpu *vcpu)
{
	struct perf_ctrl *pctrl = this_cpu_ptr(&perf_ctrl);
	struct vmexit_perf *perf;

	if (!pctrl->on)
		return;

	perf = vcpu_to_perf(vcpu);
	if (pctrl->age != perf->age)
		refresh_vmexit_perf(pctrl, perf);

	perf->rax = vcpu->arch.regs[VCPU_REGS_RAX];

	perf->tsc = pkvm_rdtsc_ordered();
}

void trace_vmexit_end(struct kvm_vcpu *vcpu, u32 reason)
{
	struct perf_ctrl *pctrl = this_cpu_ptr(&perf_ctrl);
	struct vmexit_perf *perf;
	unsigned long long cycles;

	if (!pctrl->on)
		return;

	perf = vcpu_to_perf(vcpu);
	if (pctrl->age != perf->age) {
		refresh_vmexit_perf(pctrl, perf);
		return;
	}

	if (reason >= MAX_EXIT_REASONS)
		return;

	cycles = pkvm_rdtsc_ordered() - perf->tsc;

	pkvm_spin_lock(&perf->lock);

	perf->data.vmexit.reasons[reason].count++;
	perf->data.vmexit.reasons[reason].cycles += cycles;
	perf->data.vmexit.total.count++;
	perf->data.vmexit.total.cycles += cycles;

	if (is_host_vcpu(vcpu) && reason == EXIT_REASON_VMCALL &&
	    perf->rax < MAX_PKVM_HYPERCALLS) {
		perf->data.vmexit.hypercalls[perf->rax].count++;
		perf->data.vmexit.hypercalls[perf->rax].cycles += cycles;
	}

	pkvm_spin_unlock(&perf->lock);
}

void pkvm_handle_set_vmexit_trace(bool en)
{
	struct perf_ctrl *pctrl = this_cpu_ptr(&perf_ctrl);
	int cpu = raw_smp_processor_id();

	if (en && !pctrl->on) {
		pctrl->age++;
		pctrl->on = true;
		pkvm_dbg("%s: CPU%d enable vmexit_trace\n", __func__, cpu);
		return;
	}

	if (!en && pctrl->on) {
		pctrl->on = false;
		pkvm_dbg("%s: CPU%d disable vmexit_trace\n", __func__, cpu);
		return;
	}
}

static void copy_vmexit_perf_data(struct perf_data *dst, struct vmexit_perf *perf)
{
	pkvm_spin_lock(&perf->lock);
	memcpy(dst, &perf->data, sizeof(struct perf_data));
	pkvm_spin_unlock(&perf->lock);
}

static void *copy_host_vm_trace(void *dst, unsigned long *size)
{
	struct vmexit_perf *perf;
	int cpu;

	for_each_possible_cpu(cpu) {
		perf = per_cpu_ptr(&hvcpu_perf, cpu);
		if (*size >= sizeof(struct perf_data)) {
			copy_vmexit_perf_data(dst, perf);
			dst += sizeof(struct perf_data);
			*size -= sizeof(struct perf_data);
		}
	}

	return dst;
}

struct copy_arg {
	int vm_handle;
	void *dst;
	unsigned long size;
};

static int copy_pkvm_vm_trace(struct pkvm_vm *vm, void *param)
{
	struct copy_arg *arg = param;
	struct vmexit_perf *perf;
	int i;

	if (arg->vm_handle && (arg->vm_handle != to_kvm(vm)->arch.pkvm.pkvm_vm_handle))
		return 0;

	pkvm_spin_lock(&vm->lock);
	for (i = 0; i < to_kvm(vm)->created_vcpus; i++) {
		if (!vm->vcpus[i])
			continue;
		perf = &vm->vcpus[i]->perf;

		if (arg->size >= sizeof(struct perf_data)) {
			copy_vmexit_perf_data(arg->dst, perf);
			arg->dst += sizeof(struct perf_data);
			arg->size -= sizeof(struct perf_data);
		} else {
			break;
		}
	}
	pkvm_spin_unlock(&vm->lock);

	/*
	 * If vm_handle == 0, the pkvm will continue to walk for the rest of VMs.
	 * If vm_handle !=0, means the host wants to get the trace for specific
	 * vm. The pkvm will stop walking for this case.
	 */
	return arg->vm_handle;
}

static void copy_guest_vm_trace(int vm_handle, void *dst, unsigned long size)
{
	struct copy_arg arg = {
		.vm_handle = vm_handle,
		.dst = dst,
		.size = size,
	};

	pkvm_walk_each_vm(copy_pkvm_vm_trace, &arg);
}

void pkvm_handle_dump_vmexit_trace(int vm_handle, unsigned long pa, unsigned long size)
{
	void *dst;

	if (!VALID_PAGE(pa))
		return;

	/*
	 * TODO: Assume the memory pages represented by pa is shared by
	 * the host. Pin before accessing, and unpin after.
	 */
	dst = __pkvm_va(pa);

	/*
	 * Copy host_vcpu perf data first as this will be dumpped first by
	 * the host. Then the guest perf data.
	 */
	if (!vm_handle)
		dst = copy_host_vm_trace(dst, &size);

	copy_guest_vm_trace(vm_handle, dst, size);
}

void pkvm_vcpu_perf_init(struct kvm_vcpu *vcpu)
{
	struct vmexit_perf *perf;

	if (this_cpu_read(host_vcpu) == vcpu) {
		perf = this_cpu_ptr(&hvcpu_perf);
	} else {
		perf = &to_pkvm_vcpu(vcpu)->perf;
		perf->data.vm_handle = vcpu->kvm->arch.pkvm.pkvm_vm_handle;
	}
	perf->data.vcpu_id = vcpu->vcpu_id;
}
