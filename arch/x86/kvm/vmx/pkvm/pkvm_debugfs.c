/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/debugfs.h>
#include <linux/kvm_host.h>
#include <asm/vmx.h>
#include <asm/kvm_para.h>
#include <asm/kvm_pkvm.h>
#include <pkvm_trace.h>
#include <pkvm_debugfs.h>

static void set_vmexit_trace_func(void *data)
{
	u64 val;

	if (!data)
		return;

	val = *(u64 *)data;
	pkvm_hypercall(set_vmexit_trace, val);
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

static const char *pkvm_hypercalls[MAX_PKVM_HYPERCALLS] = {
	#define PKVM_HC(fn)	[TO_PKVM_HC(fn)] = #fn,
	#include <asm/pkvm_hypercalls.h>
};

static void print_vmexit_stats(struct seq_file *m, struct vmexit_stats *st)
{
	seq_printf(m, "%lld cycles %lld each-handler-cycle %lld\n",
		   st->count, st->cycles, st->cycles / st->count);
}

static void dump_perf_data(struct seq_file *m, struct perf_data *perf,
			   struct perf_data *summary)
{
	int i;

	for (i = 0 ; i < MAX_EXIT_REASONS; i++) {
		if (!perf->vmexit.reasons[i].count)
			continue;

		if (perf->vm_handle)
			seq_printf(m, "VM%d-vcpu%d: %s ", perf->vm_handle, perf->vcpu_id,
				   get_vmexit_reason(i));
		else
			seq_printf(m, "Host-vcpu%d: %s ", perf->vcpu_id, get_vmexit_reason(i));
		print_vmexit_stats(m, &perf->vmexit.reasons[i]);

		summary->vmexit.reasons[i].count += perf->vmexit.reasons[i].count;
		summary->vmexit.reasons[i].cycles += perf->vmexit.reasons[i].cycles;

		if (need_resched())
			cond_resched();
	}

	for (i = 0; i < MAX_PKVM_HYPERCALLS; i++) {
		if (!perf->vmexit.hypercalls[i].count)
			continue;

		if (perf->vm_handle)
			seq_printf(m, "VM%d-vcpu%d: %s ", perf->vm_handle, perf->vcpu_id,
				   pkvm_hypercalls[i]);
		else
			seq_printf(m, "Host-vcpu%d: %s ", perf->vcpu_id, pkvm_hypercalls[i]);
		print_vmexit_stats(m, &perf->vmexit.hypercalls[i]);

		summary->vmexit.hypercalls[i].count += perf->vmexit.hypercalls[i].count;
		summary->vmexit.hypercalls[i].cycles += perf->vmexit.hypercalls[i].cycles;

		if (need_resched())
			cond_resched();
	}
}

static void print_summary(struct seq_file *m, struct perf_data *summary)
{
	int i;

	for (i = 0 ; i < MAX_EXIT_REASONS; i++) {
		if (!summary->vmexit.reasons[i].count)
			continue;

		if (summary->vm_handle)
			seq_printf(m, "VM%d: %s ", summary->vm_handle, get_vmexit_reason(i));
		else
			seq_printf(m, "Host: %s ", get_vmexit_reason(i));
		print_vmexit_stats(m, &summary->vmexit.reasons[i]);

		if (need_resched())
			cond_resched();
	}

	for (i = 0; i < MAX_PKVM_HYPERCALLS; i++) {
		if (!summary->vmexit.hypercalls[i].count)
			continue;

		if (summary->vm_handle)
			seq_printf(m, "VM%d: %s ", summary->vm_handle, pkvm_hypercalls[i]);
		else
			seq_printf(m, "Host: %s ", pkvm_hypercalls[i]);
		print_vmexit_stats(m, &summary->vmexit.hypercalls[i]);

		if (need_resched())
			cond_resched();
	}
}

static struct perf_data *dump_host_vcpu_perf_data(struct seq_file *m,
						  struct perf_data *dump,
						  struct perf_data *summary,
						  unsigned long *size)
{
	struct perf_data *perf = dump;
	int cpu;

	/* Not a host vcpu perf data */
	if (perf->vm_handle)
		return perf;

	memset(summary, 0, sizeof(struct perf_data));

	for (cpu = 0;
	     cpu < num_possible_cpus() && *size >= sizeof(struct perf_data);
	     cpu++, *size -= sizeof(struct perf_data), perf++)
		dump_perf_data(m, perf, summary);

	print_summary(m, summary);

	return perf;
}

static void dump_guest_vcpu_perf_data(struct seq_file *m, struct perf_data *dump,
				      struct perf_data *summary, unsigned long size)
{
	struct perf_data *perf;

	if (size < sizeof(struct perf_data))
		return;

	memset(summary, 0, sizeof(struct perf_data));

	for (perf = dump, summary->vm_handle = perf->vm_handle;
	     size >= sizeof(struct perf_data);
	     size -= sizeof(struct perf_data), perf++) {
		/* Should have no host vcpu perf data already */
		if (WARN_ON_ONCE(perf->vm_handle == 0))
			continue;

		if (summary->vm_handle != perf->vm_handle) {
			/* Start to dump another VM, print summary */
			print_summary(m, summary);
			memset(summary, 0, sizeof(struct perf_data));
			summary->vm_handle = perf->vm_handle;
		}

		dump_perf_data(m, perf, summary);
	}

	print_summary(m, summary);
}

static void pkvm_dump_vmexit_trace(struct seq_file *m, struct perf_data *dump,
				   struct perf_data *summary, unsigned long size)
{
	/* Try to dump host vcpu perf as this is first copied by the pkvm */
	struct perf_data *guest_perf = dump_host_vcpu_perf_data(m, dump, summary, &size);

	dump_guest_vcpu_perf_data(m, guest_perf, summary, size);
}

static int vmexit_trace_show(struct seq_file *m, void *unused)
{
	struct kvm *kvm = (struct kvm *)m->private;
	struct perf_data *perf, *summary;
	unsigned long size;
	int vm_handle = 0;

	if (kvm) {
		/* Dump vmexit trace for a specific VM */
		size = atomic_read(&kvm->online_vcpus) * sizeof(struct perf_data);
		vm_handle = kvm->arch.pkvm.pkvm_vm_handle;
	} else {
		/* Dump vmexit trace for all VMs including the host VM */
		size = sizeof(struct perf_data) * num_possible_cpus();
		mutex_lock(&kvm_lock);
		list_for_each_entry(kvm, &vm_list, vm_list)
			size += atomic_read(&kvm->online_vcpus) * sizeof(struct perf_data);
		mutex_unlock(&kvm_lock);
	}

	perf = alloc_pages_exact(size, GFP_KERNEL_ACCOUNT);
	if (!perf) {
		pr_err("Failed to allocate perf buffer\n");
		return -ENOMEM;
	}

	summary = kmalloc(sizeof(struct perf_data), GFP_KERNEL_ACCOUNT);
	if (!summary) {
		pr_err("Failed to allocate perf summary buffer\n");
		free_pages_exact(perf, size);
		return -ENOMEM;
	}

	/*TODO: Share perf memory with the pkvm hypervisor */

	pkvm_hypercall(dump_vmexit_trace, vm_handle, __pa(perf), size);

	/*TODO: Unshare perf memory with the pkvm hypervisor */

	pkvm_dump_vmexit_trace(m, perf, summary, size);

	kfree(summary);
	free_pages_exact(perf, size);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vmexit_trace);

struct debugfs_item {
	const char *name;
	umode_t mode;
	const  struct file_operations *fops;
	struct dentry *dentry;
};

struct debugfs_item debugfs_files[] = {
	{ "set_vmexit_trace", 0222, &set_vmexit_trace_fops},
	{ "vmexit_trace", 0444, &vmexit_trace_fops},
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

void pkvm_create_vm_debugfs(struct kvm *kvm)
{
	debugfs_create_file("pkvm_vmexit_trace", 0444, kvm->debugfs_dentry,
			    kvm, &vmexit_trace_fops);
}
