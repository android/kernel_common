// SPDX-License-Identifier: GPL-2.0
#include <linux/align.h>
#include <asm/processor.h>
#include <asm/sections.h>
#include <asm/kvm_pkvm.h>
#include <asm/percpu.h>
#include <asm/page.h>
#include "cpu.h"

unsigned long __per_cpu_offset[NR_CPUS];
DEFINE_PER_CPU_READ_MOSTLY(unsigned long, this_cpu_off);
DEFINE_PER_CPU_ALIGNED(struct pcpu_hot, pcpu_hot);
DEFINE_PER_CPU(u64, x86_spec_ctrl_current);

struct cpumask __cpu_possible_mask __ro_after_init;
struct cpuinfo_x86 boot_cpu_data;
unsigned int nr_cpu_ids;
unsigned int tsc_khz;
u64 x86_pred_cmd;

unsigned int pkvm_per_cpu_nr_pages(void)
{
	unsigned long per_cpu_size = (unsigned long)__per_cpu_end -
				     (unsigned long)__per_cpu_start;

	return ALIGN(per_cpu_size, PAGE_SIZE) >> PAGE_SHIFT;
}

int setup_pkvm_per_cpu(int cpu, unsigned long base)
{
	unsigned long elf_base;

	if (cpu >= ARRAY_SIZE(__per_cpu_offset))
		return -EINVAL;

	elf_base = (unsigned long)__per_cpu_start;
	__per_cpu_offset[cpu] = (unsigned long)__pkvm_va(base) - elf_base;
	per_cpu(this_cpu_off, cpu) = __per_cpu_offset[cpu];
	per_cpu(pcpu_hot.cpu_number, cpu) = cpu;

	return 0;
}

void warn_thunk_thunk(void)
{
	WARN_ONCE(1, "pkvm: Unpatched return thunk in use. This should not happen!\n");
}
