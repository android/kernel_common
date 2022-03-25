// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef _PKVM_H_
#define _PKVM_H_

#include <asm/pkvm_image.h>
#include <vmx/vmx.h>

#define STACK_SIZE SZ_16K

struct pkvm_pgtable_cap {
	int level;
	int allowed_pgsz;
	u64 table_prot;
};

struct idt_page {
	gate_desc idt[IDT_ENTRIES];
} __aligned(PAGE_SIZE);

struct pkvm_pcpu {
	u8 stack[STACK_SIZE] __aligned(16);
	unsigned long cr3;
	struct gdt_page gdt_page;
	struct idt_page idt_page;
	struct tss_struct tss;
};

struct pkvm_host_vcpu {
	struct vcpu_vmx vmx;
	struct pkvm_pcpu *pcpu;
	struct vmcs *vmxarea;
};

struct pkvm_host_vm {
	struct pkvm_host_vcpu *host_vcpus[CONFIG_NR_CPUS];
};

struct pkvm_hyp {
	int num_cpus;

	struct vmx_capability vmx_cap;
	struct vmcs_config vmcs_config;

	struct pkvm_pgtable_cap mmu_cap;
	struct pkvm_pgtable_cap ept_cap;

	struct pkvm_pcpu *pcpus[CONFIG_NR_CPUS];

	struct pkvm_host_vm host_vm;
};

#define PKVM_PAGES (ALIGN(sizeof(struct pkvm_hyp), PAGE_SIZE) >> PAGE_SHIFT)
#define PKVM_PCPU_PAGES (ALIGN(sizeof(struct pkvm_pcpu), PAGE_SIZE) >> PAGE_SHIFT)
#define PKVM_HOST_VCPU_PAGES (ALIGN(sizeof(struct pkvm_host_vcpu), PAGE_SIZE) >> PAGE_SHIFT)
#define PKVM_VMCS_PAGES 3 /*vmxarea+vmcs+msr_bitmap*/
#define PKVM_PERCPU_PAGES (PKVM_PCPU_PAGES + PKVM_HOST_VCPU_PAGES + PKVM_VMCS_PAGES)

extern char __pkvm_text_start[], __pkvm_text_end[];

extern unsigned long pkvm_sym(__page_base_offset);
extern unsigned long pkvm_sym(__symbol_base_offset);

PKVM_DECLARE(void, __pkvm_vmx_vmexit(void));
PKVM_DECLARE(int, pkvm_main(struct kvm_vcpu *vcpu));

PKVM_DECLARE(void *, pkvm_early_alloc_contig(unsigned int nr_pages));
PKVM_DECLARE(void *, pkvm_early_alloc_page(void));
PKVM_DECLARE(void, pkvm_early_alloc_init(void *virt, unsigned long size));

#endif
