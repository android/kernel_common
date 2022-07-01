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
	struct vmcs *current_vmcs;

	bool pending_nmi;
};

struct pkvm_host_vm {
	struct pkvm_host_vcpu *host_vcpus[CONFIG_NR_CPUS];
	struct pkvm_pgtable *ept;
};

struct pkvm_hyp {
	int num_cpus;

	struct vmx_capability vmx_cap;
	struct vmcs_config vmcs_config;

	struct pkvm_pgtable_cap mmu_cap;
	struct pkvm_pgtable_cap ept_cap;

	struct pkvm_pgtable *mmu;

	struct pkvm_pcpu *pcpus[CONFIG_NR_CPUS];

	struct pkvm_host_vm host_vm;
};

static inline struct pkvm_host_vcpu *vmx_to_pkvm_hvcpu(struct vcpu_vmx *vmx)
{
	return container_of(vmx, struct pkvm_host_vcpu, vmx);
}

static inline struct pkvm_host_vcpu *to_pkvm_hvcpu(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	return vmx_to_pkvm_hvcpu(vmx);
}

struct pkvm_section {
	unsigned long type;
#define PKVM_RESERVED_MEMORY		0UL
#define PKVM_CODE_DATA_SECTIONS		1UL
#define KERNEL_DATA_SECTIONS		2UL
	unsigned long addr;
	unsigned long size;
	u64 prot;
};

#define PKVM_PAGES (ALIGN(sizeof(struct pkvm_hyp), PAGE_SIZE) >> PAGE_SHIFT)
#define PKVM_PCPU_PAGES (ALIGN(sizeof(struct pkvm_pcpu), PAGE_SIZE) >> PAGE_SHIFT)
#define PKVM_HOST_VCPU_PAGES (ALIGN(sizeof(struct pkvm_host_vcpu), PAGE_SIZE) >> PAGE_SHIFT)
#define PKVM_VMCS_PAGES 3 /*vmxarea+vmcs+msr_bitmap*/
#define PKVM_PERCPU_PAGES (PKVM_PCPU_PAGES + PKVM_HOST_VCPU_PAGES + PKVM_VMCS_PAGES)

extern char __pkvm_text_start[], __pkvm_text_end[];
extern char __pkvm_rodata_start[], __pkvm_rodata_end[];
extern char __pkvm_data_start[], __pkvm_data_end[];
extern char __pkvm_bss_start[], __pkvm_bss_end[];

extern unsigned long pkvm_sym(__page_base_offset);
extern unsigned long pkvm_sym(__symbol_base_offset);
extern struct pkvm_hyp *pkvm_sym(pkvm_hyp);

PKVM_DECLARE(void, __pkvm_vmx_vmexit(void));
PKVM_DECLARE(int, pkvm_main(struct kvm_vcpu *vcpu));
PKVM_DECLARE(void, pkvm_init_host_state_area(struct pkvm_pcpu *pcpu, int cpu));

PKVM_DECLARE(void *, pkvm_early_alloc_contig(unsigned int nr_pages));
PKVM_DECLARE(void *, pkvm_early_alloc_page(void));
PKVM_DECLARE(void, pkvm_early_alloc_init(void *virt, unsigned long size));

PKVM_DECLARE(void, noop_handler(void));
PKVM_DECLARE(void, nmi_handler(void));

#endif
