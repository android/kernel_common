// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#ifndef _ASM_X86_KVM_PKVM_H
#define _ASM_X86_KVM_PKVM_H

#include <linux/kvm_host.h>

#ifdef CONFIG_PKVM_INTEL

#include <linux/memblock.h>
#include <asm/pkvm_image.h>

#define HYP_MEMBLOCK_REGIONS   128
#define PKVM_PGTABLE_MAX_LEVELS		5U

/* PKVM Hypercalls */
#define PKVM_HC_INIT_FINALISE		1
#define PKVM_HC_INIT_SHADOW_VM		2
#define PKVM_HC_INIT_SHADOW_VCPU	3
#define PKVM_HC_TEARDOWN_SHADOW_VM	4
#define PKVM_HC_TEARDOWN_SHADOW_VCPU	5

extern struct memblock_region pkvm_sym(hyp_memory)[];
extern unsigned int pkvm_sym(hyp_memblock_nr);

void *pkvm_phys_to_virt(unsigned long phys);
unsigned long pkvm_virt_to_phys(void *virt);

#define __pkvm_pa(virt)	pkvm_virt_to_phys((void *)(virt))
#define __pkvm_va(phys)	pkvm_phys_to_virt((unsigned long)(phys))

/*TODO: unify the API name: __pkvm vs. __hyp? */
#define __hyp_pa __pkvm_pa
#define __hyp_va __pkvm_va

extern phys_addr_t hyp_mem_base;
extern phys_addr_t hyp_mem_size;

void __init kvm_hyp_reserve(void);

static inline unsigned long __pkvm_pgtable_max_pages(unsigned long nr_pages)
{
	unsigned long total = 0, i;

	/* Provision the worst case */
	for (i = 0; i < PKVM_PGTABLE_MAX_LEVELS; i++) {
		nr_pages = DIV_ROUND_UP(nr_pages, PTRS_PER_PTE);
		total += nr_pages;
	}

	return total;
}

static inline unsigned long __pkvm_pgtable_total_pages(void)
{
	unsigned long total = 0, i;

	for (i = 0; i < pkvm_sym(hyp_memblock_nr); i++) {
		struct memblock_region *reg = &pkvm_sym(hyp_memory)[i];

		total += __pkvm_pgtable_max_pages(reg->size >> PAGE_SHIFT);
	}

	return total;
}

static inline unsigned long host_ept_pgtable_pages(void)
{
	unsigned long res;

	/*
	 * Include an extra 16 pages to safely upper-bound the worst case of
	 * concatenated pgds.
	 */
	res = __pkvm_pgtable_total_pages() + 16;

	/* Allow 1 GiB for MMIO mappings */
	 res += __pkvm_pgtable_max_pages(SZ_1G >> PAGE_SHIFT);

	return res;
}

static inline unsigned long pkvm_mmu_pgtable_pages(void)
{
	unsigned long res;

	res = __pkvm_pgtable_total_pages();

	return res;
}

static inline unsigned long pkvm_vmemmap_memblock_size(struct memblock_region *reg,
		size_t vmemmap_entry_size)
{
	unsigned long nr_pages = reg->size >> PAGE_SHIFT;
	unsigned long start, end;

	/* Translate the pfn to the vmemmap entry */
	start = (reg->base >> PAGE_SHIFT) * vmemmap_entry_size;
	end = start + nr_pages * vmemmap_entry_size;
	start = ALIGN_DOWN(start, PAGE_SIZE);
	end = ALIGN(end, PAGE_SIZE);

	return end - start;
}

static inline unsigned long pkvm_vmemmap_pages(size_t vmemmap_entry_size)
{
	unsigned long total_size = 0, i;

	for (i = 0; i < pkvm_sym(hyp_memblock_nr); i++) {
		total_size += pkvm_vmemmap_memblock_size(&pkvm_sym(hyp_memory)[i],
							 vmemmap_entry_size);
	}

	return total_size >> PAGE_SHIFT;
}

static inline unsigned long pkvm_data_struct_pages(unsigned long global_pgs,
		unsigned long percpu_pgs, int num_cpus)
{
	return (percpu_pgs * num_cpus + global_pgs);
}

static inline int hyp_pre_reserve_check(void)
{
	/* no necessary check yet*/
	return 0;
}

u64 hyp_total_reserve_pages(void);

int pkvm_init_shadow_vm(struct kvm *kvm);
void pkvm_teardown_shadow_vm(struct kvm *kvm);
int pkvm_init_shadow_vcpu(struct kvm_vcpu *vcpu);
void pkvm_teardown_shadow_vcpu(struct kvm_vcpu *vcpu);
#else
static inline void kvm_hyp_reserve(void) {}
static inline int pkvm_init_shadow_vm(struct kvm *kvm) { return 0; }
static inline void pkvm_teardown_shadow_vm(struct kvm *kvm) {}
static inline int pkvm_init_shadow_vcpu(struct kvm_vcpu *vcpu) { return 0; }
static inline void pkvm_teardown_shadow_vcpu(struct kvm_vcpu *vcpu) {}
#endif

#endif
