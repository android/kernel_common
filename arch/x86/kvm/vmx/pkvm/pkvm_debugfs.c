/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/debugfs.h>
#include <asm/vmx.h>
#include <asm/kvm_para.h>
#include <pkvm_trace.h>

static void set_vmexit_trace_func(void *data)
{
	u64 val;

	if (!data)
		return;

	val = *(u64 *)data;
	kvm_hypercall1(PKVM_HC_SET_VMEXIT_TRACE, val);
}

static int set_vmexit_trace(void *data, u64 val)
{
	int cpu;

	for_each_possible_cpu(cpu)
		smp_call_function_single(cpu, set_vmexit_trace_func, &val, true);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(set_vmexit_trace_fops, NULL, set_vmexit_trace, "%llu\n");

static struct trace_print_flags vmexit_reasons[] = { VMX_EXIT_REASONS, { -1, NULL }};

static const char *get_vmexit_reason(int index)
{
	struct trace_print_flags *p = vmexit_reasons;

	while (p->name) {
		if (p->mask == index)
			return p->name;
		p++;
	}

	return NULL;
}

static void __pkvm_vmexit_perf_dump_percpu(struct vmexit_perf_dump *perf,
					   struct vmexit_perf_dump *count,
					   bool dump_l2)
{
	struct perf_data *perf_data, *count_perf_data;
	int cpu = perf->cpu;
	int i;

	if (dump_l2) {
		perf_data = &perf->l2data;
		count_perf_data = count ? &count->l2data : NULL;
	} else {
		perf_data = &perf->l1data;
		count_perf_data = count ? &count->l1data : NULL;
	}

	for (i = 0 ; i < 74; i++) {
		if (!perf_data->data.reasons[i])
			continue;

		pr_info("CPU%d vmexit_from_%s reason %s %lld cycles %lld each-handler-cycle %lld\n",
			  cpu, dump_l2 ? "l2" : "l1", get_vmexit_reason(i),
			  perf_data->data.reasons[i], perf_data->data.cycles[i],
			  perf_data->data.cycles[i] / perf_data->data.reasons[i]);

		if (count_perf_data) {
			count_perf_data->data.reasons[i] += perf_data->data.reasons[i];
			count_perf_data->data.cycles[i] += perf_data->data.cycles[i];
		}

		if (need_resched())
			cond_resched();
	}

	if (perf_data->data.total_count) {
		pr_info("CPU%d total_vmexit_from_%s %lld total_cycles %lld\n",
			  cpu, dump_l2 ? "l2" : "l1",
			  perf_data->data.total_count,
			  perf_data->data.total_cycles);
		memset(perf_data, 0, sizeof(struct perf_data));
	}
}

static void __pkvm_vmexit_perf_dump_summary(struct vmexit_perf_dump *perf, bool dump_l2)
{
	struct perf_data *perf_data;
	int i;

	if (dump_l2)
		perf_data = &perf->l2data;
	else
		perf_data = &perf->l1data;

	for (i = 0 ; i < 74; i++) {
		if (!perf_data->data.reasons[i])
			continue;

		pr_info("AllCPU: vmexit_from_%s reason %s %lld cycles %lld each-handler-cycle %lld\n",
			  dump_l2 ? "l2" : "l1", get_vmexit_reason(i),
			  perf_data->data.reasons[i], perf_data->data.cycles[i],
			  perf_data->data.cycles[i] / perf_data->data.reasons[i]);

		perf_data->data.total_count += perf_data->data.reasons[i];
		perf_data->data.total_cycles += perf_data->data.cycles[i];

		if (need_resched())
			cond_resched();
	}

	pr_info("AllCPU: total_vmexit_from_%s %lld total_cycles %lld\n",
		  dump_l2 ? "l2" : "l1",
		  perf_data->data.total_count,
		  perf_data->data.total_cycles);
}

static struct vmexit_perf_dump pkvm_perf;
static void pkvm_dump_vmexit_trace(struct vmexit_perf_dump *hvcpu_perf)
{
	struct vmexit_perf_dump *perf;
	int cpu;

	memset(&pkvm_perf.l1data, 0, sizeof(struct perf_data));
	memset(&pkvm_perf.l2data, 0, sizeof(struct perf_data));

	for (cpu = 0; cpu < num_possible_cpus(); cpu++) {
		perf = &hvcpu_perf[cpu];

		__pkvm_vmexit_perf_dump_percpu(perf, &pkvm_perf, false);
		__pkvm_vmexit_perf_dump_percpu(perf, &pkvm_perf, true);
	}

	__pkvm_vmexit_perf_dump_summary(&pkvm_perf, false);
	__pkvm_vmexit_perf_dump_summary(&pkvm_perf, true);
}

static int dump_vmexit_trace(void *data, u64 *val)
{
	struct vmexit_perf_dump *hvcpu_perf;
	unsigned long size = sizeof(struct vmexit_perf_dump) * num_possible_cpus();

	hvcpu_perf = alloc_pages_exact(size, GFP_KERNEL_ACCOUNT);

	kvm_hypercall2(PKVM_HC_DUMP_VMEXIT_TRACE, __pa(hvcpu_perf), size);
	barrier();

	pkvm_dump_vmexit_trace(hvcpu_perf);

	free_pages_exact(hvcpu_perf, size);

	*val = 0;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(dump_vmexit_trace_fops, dump_vmexit_trace, NULL, "%llu\n");

struct debugfs_item {
	const char *name;
	umode_t mode;
	const  struct file_operations *fops;
	struct dentry *dentry;
};

struct debugfs_item debugfs_files[] = {
	{ "set_vmexit_trace", 0222, &set_vmexit_trace_fops},
	{ "dump_vmexit_trace", 0444, &dump_vmexit_trace_fops},
	{ NULL }
};

static struct dentry *debugfs_dir;

void pkvm_init_debugfs(void)
{
	struct debugfs_item *p;

	debugfs_dir = debugfs_create_dir("pkvm", NULL);
	if (IS_ERR_OR_NULL(debugfs_dir)) {
		pr_err("MCP_TEST: Can't create debugfs root entry\n");
		goto failed_dir;
	}

	for (p = debugfs_files; p->name; ++p) {
		p->dentry = debugfs_create_file(p->name, p->mode,
						debugfs_dir,
						NULL, p->fops);
		if (IS_ERR_OR_NULL(p->dentry))
			goto out_dir;
	}

	return;

out_dir:
	for (p = debugfs_files; p->dentry; ++p) {
		debugfs_remove(p->dentry);
		p->dentry = NULL;
	}
	debugfs_remove(debugfs_dir);
failed_dir:
	debugfs_dir = NULL;
}
