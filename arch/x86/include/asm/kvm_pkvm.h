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
#include <asm/pkvm.h>

#define HYP_MEMBLOCK_REGIONS   128
#define PKVM_PGTABLE_MAX_LEVELS		5U

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

/*
 * Calculate the total pages for shadow EPT with assumptions:
 * 1. there is no shared memory between each VMs (normal or protected).
 * 2. all VMs total memory size is no larger than the platform memory size.
 * 3. the virtual MMIO range for each VM is no larger than 1G.
 */
static inline unsigned long pkvm_shadow_ept_pgtable_pages(int nr_vm)
{
	unsigned long pgtable_pages = __pkvm_pgtable_total_pages();
	unsigned long res;

	res = pgtable_pages;

	/*
	 * Above 'res' covered enough pages for a shadow EPT to create all
	 * possible levels mapping for the whole platform memory size. The pages
	 * reserved for lvl-1 mapping (usually 4K size) actually can be shared
	 * among all VM shadow EPTs as their occupied memory are partitioned
	 * according to the assumption. While for lvl-2,3,4 or possible lvl-5
	 * mappings, above 'res' pages only covered for one shadow EPT, we need
	 * reserve more for other 'nr_vm -1' VMs.
	 *
	 * The lvl-2 to lvl-5 pages for one VM's shadow EPT can be approximately
	 * got by:
	 *     __pkvm_pgtable_total_pages()/PTRS_PER_PTE
	 */
	res += DIV_ROUND_UP(pgtable_pages, PTRS_PER_PTE) * (nr_vm - 1);

	/* Allow 1 GiB for MMIO mappings for each VM */
	 res += __pkvm_pgtable_max_pages(SZ_1G >> PAGE_SHIFT) * nr_vm;

	return res;
}

u64 hyp_total_reserve_pages(void);

int pkvm_init_shadow_vm(struct kvm *kvm);
void pkvm_teardown_shadow_vm(struct kvm *kvm);
int pkvm_init_shadow_vcpu(struct kvm_vcpu *vcpu);
void pkvm_teardown_shadow_vcpu(struct kvm_vcpu *vcpu);
int pkvm_tlb_remote_flush(struct kvm *kvm);
int pkvm_tlb_remote_flush_with_range(struct kvm *kvm,
				     gfn_t gfn, gfn_t nr_pages);
#else
static inline void kvm_hyp_reserve(void) {}
static inline int pkvm_init_shadow_vm(struct kvm *kvm) { return 0; }
static inline void pkvm_teardown_shadow_vm(struct kvm *kvm) {}
static inline int pkvm_init_shadow_vcpu(struct kvm_vcpu *vcpu) { return 0; }
static inline void pkvm_teardown_shadow_vcpu(struct kvm_vcpu *vcpu) {}
#endif

#endif
