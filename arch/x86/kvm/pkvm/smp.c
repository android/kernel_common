// SPDX-License-Identifier: GPL-2.0
#include <linux/align.h>
#include <asm/processor.h>
#include <asm/sections.h>
#include <asm/kvm_pkvm.h>
#include <asm/percpu.h>
#include <asm/page.h>
#include "smp.h"

unsigned long __per_cpu_offset[NR_CPUS];

unsigned int pkvm_per_cpu_nr_pages(void)
{
	/* TODO: calculate number of pages for pkvm percpu */
	return 0;
}

int setup_pkvm_per_cpu(int cpu, unsigned long base)
{
	if (cpu >= ARRAY_SIZE(__per_cpu_offset))
		return -EINVAL;
	/*
	 * TODO: calculate the pkvm percpu offset based on
	 * percpu section address
	 */
	__per_cpu_offset[cpu] = (unsigned long)__pkvm_va(base);

	return 0;
}
