/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/kvm_host.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>
#include "trace.h"

/*
 * memset/memcpy can be re-defined by include/linux/fortify-string.h, which
 * may introduce additional linux kernel symbols. Undefine them to force
 * use the implementation in pkvm/hyp/lib/
 * */
#undef memset
#undef memcpy

struct vmexit_perf {
	struct perf_data l1data;
	struct perf_data l2data;
	struct perf_data *cur;
	bool on;
	bool start;
	int cpu;
	pkvm_spinlock_t lock;
};
static struct vmexit_perf hvcpu_perf[CONFIG_NR_CPUS];

static inline unsigned long long pkvm_rdtsc_ordered(void)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("lfence;rdtsc" : EAX_EDX_RET(val, low, high));

	return EAX_EDX_VAL(val, low, high);
}

void trace_vmexit_start(struct kvm_vcpu *vcpu, bool nested_vmexit)
{
	int cpu = vcpu->cpu;
	struct vmexit_perf *perf = &hvcpu_perf[cpu];

	if (!perf->on)
		return;

	perf->start = true;
	perf->cpu = cpu;
	if (nested_vmexit)
		perf->cur = &perf->l2data;
	else
		perf->cur = &perf->l1data;

	pkvm_spin_lock(&perf->lock);
	perf->cur->tsc = pkvm_rdtsc_ordered();
	pkvm_spin_unlock(&perf->lock);
}

void trace_vmexit_end(struct kvm_vcpu *vcpu, u32 index)
{
	int cpu = vcpu->cpu;
	struct vmexit_perf *perf = &hvcpu_perf[cpu];
	struct perf_data *perf_data = perf->cur;
	unsigned long long cycles;

	if (!perf->on || !perf->start || !perf_data)
		return;

	pkvm_spin_lock(&perf->lock);
	cycles = pkvm_rdtsc_ordered() - perf_data->tsc;
	perf_data->data.cycles[index] += cycles;
	perf_data->data.total_cycles += cycles;
	perf_data->data.total_count++;
	perf_data->data.reasons[index]++;
	pkvm_spin_unlock(&perf->lock);
}

void pkvm_handle_set_vmexit_trace(struct kvm_vcpu *vcpu, bool en)
{
	int cpu = vcpu->cpu;
	struct vmexit_perf *perf = &hvcpu_perf[cpu];

	if (en && !perf->on) {
		perf->on = true;
		pkvm_dbg("%s: CPU%d enable vmexit_trace\n", __func__, cpu);
		memset(&perf->l1data, 0, sizeof(struct perf_data));
		memset(&perf->l2data, 0, sizeof(struct perf_data));
		return;
	}

	if (!en && perf->on) {
		perf->on = false;
		perf->start = false;
		pkvm_dbg("%s: CPU%d disable vmexit_trace\n", __func__, cpu);
		return;
	}
}

void pkvm_handle_dump_vmexit_trace(unsigned long pa, unsigned long size)
{
	void *out = pkvm_phys_to_virt(pa);
	struct pkvm_host_vcpu *p;
	struct vmexit_perf *perf;
	int cpu, index;

	for (index = 0; index < CONFIG_NR_CPUS; index++) {
		p = pkvm_hyp->host_vm.host_vcpus[index];
		if (!p)
			continue;

		cpu = p->vmx.vcpu.cpu;
		perf = &hvcpu_perf[cpu];

		pkvm_spin_lock(&perf->lock);
		if (size >= sizeof(struct vmexit_perf_dump)) {
			struct vmexit_perf_dump *dump = out;

			memcpy(&dump->l1data, &perf->l1data, sizeof(struct perf_data));
			memcpy(&dump->l2data, &perf->l2data, sizeof(struct perf_data));
			dump->cpu = perf->cpu;
			out += sizeof(struct vmexit_perf_dump);
			size -= sizeof(struct vmexit_perf_dump);
		}
		pkvm_spin_unlock(&perf->lock);
	}
}
