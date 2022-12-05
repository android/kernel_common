// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <linux/memblock.h>
#include <mmu.h>
#include <mmu/spte.h>
#include <asm/kvm_pkvm.h>

#include <pkvm.h>
#include "pkvm_hyp.h"
#include "gfp.h"
#include "early_alloc.h"
#include "memory.h"
#include "pgtable.h"
#include "mmu.h"
#include "debug.h"

void *pkvm_mmu_pgt_base;
void *pkvm_vmemmap_base;

static int divide_memory_pool(phys_addr_t phys, unsigned long size)
{
	int data_struct_size = pkvm_data_struct_pages(PKVM_PAGES,
			PKVM_PCPU_PAGES + PKVM_HOST_VCPU_PAGES
			+ PKVM_VMCS_PAGES, pkvm_hyp->num_cpus) << PAGE_SHIFT;
	void *virt = __pkvm_va(phys + data_struct_size);
	unsigned long nr_pages;

	pkvm_early_alloc_init(virt, size - data_struct_size);

	nr_pages = pkvm_vmemmap_pages(sizeof(struct hyp_page));
	pkvm_vmemmap_base = pkvm_early_alloc_contig(nr_pages);
	if (!pkvm_vmemmap_base)
		return -ENOMEM;

	nr_pages = pkvm_mmu_pgtable_pages();
	pkvm_mmu_pgt_base = pkvm_early_alloc_contig(nr_pages);
	if (!pkvm_mmu_pgt_base)
		return -ENOMEM;

	return 0;
}

static int pkvm_back_vmemmap(phys_addr_t back_pa)
{
	unsigned long i, start, start_va, size, end, end_va = 0;
	struct memblock_region *reg;
	int ret;

	/* vmemmap region map to virtual address 0 */
	__hyp_vmemmap = 0;

	for (i = 0; i < hyp_memblock_nr; i++) {
		reg = &hyp_memory[i];
		start = reg->base;
		/* Translate a range of memory to vmemmap range */
		start_va = ALIGN_DOWN((unsigned long)hyp_phys_to_page(start),
				   PAGE_SIZE);
		/*
		 * The beginning of the pkvm_vmemmap region for the current
		 * memblock may already be backed by the page backing the end
		 * of the previous region, so avoid mapping it twice.
		 */
		start_va = max(start_va, end_va);

		end = reg->base + reg->size;
		end_va = ALIGN((unsigned long)hyp_phys_to_page(end), PAGE_SIZE);
		if (start_va >= end_va)
			continue;

		size = end_va - start_va;
		/*
		 * Create mapping for vmemmap virtual address
		 * [start, start+size) to physical address
		 * [back, back+size).
		 */
		ret = pkvm_mmu_map(start_va, back_pa, size, 0,
				  (u64)pgprot_val(PAGE_KERNEL));
		if (ret)
			return ret;

		memset(__pkvm_va(back_pa), 0, size);
		back_pa += size;
	}

	return 0;
}

static int create_mmu_mapping(const struct pkvm_section sections[],
				 int section_sz)
{
	unsigned long nr_pages = pkvm_mmu_pgtable_pages();
	struct memblock_region *reg;
	int ret, i;

	ret = pkvm_early_mmu_init(&pkvm_hyp->mmu_cap,
			pkvm_mmu_pgt_base, nr_pages);
	if (ret)
		return ret;

	/*
	 * Create mapping for the memory in memblocks.
	 * This will include all the memory host kernel can see, as well
	 * as the memory pkvm allocated during init.
	 *
	 * The virtual address for this mapping is the same with the kernel
	 * direct mapping.
	 */
	for (i = 0; i < hyp_memblock_nr; i++) {
		reg = &hyp_memory[i];
		ret = pkvm_mmu_map((unsigned long)__pkvm_va(reg->base),
				reg->base, reg->size,
				0, (u64)pgprot_val(PAGE_KERNEL));
		if (ret)
			return ret;
	}

	for (i = 0; i < section_sz; i++) {
		if (sections[i].type != PKVM_RESERVED_MEMORY) {
			ret = pkvm_mmu_map(sections[i].addr,
					__pkvm_pa_symbol(sections[i].addr),
					sections[i].size,
					0, sections[i].prot);
		}
		if (ret)
			return ret;
	}

	ret = pkvm_back_vmemmap(__pkvm_pa(pkvm_vmemmap_base));
	if (ret)
		return ret;

	/* Switch the mmu pgtable to enable pkvm_vmemmap */
	native_write_cr3(pkvm_hyp->mmu->root_pa);

	pkvm_later_mmu_init(pkvm_mmu_pgt_base, nr_pages);

	return 0;
}

#define TMP_SECTION_SZ	16UL
int __pkvm_init_finalise(struct kvm_vcpu *vcpu, struct pkvm_section sections[],
			 int section_sz)
{
	int i, ret = 0;
	static bool pkvm_init;
	struct pkvm_host_vcpu *pkvm_host_vcpu = to_pkvm_hvcpu(vcpu);
	struct pkvm_pcpu *pcpu = pkvm_host_vcpu->pcpu;
	struct pkvm_section tmp_sections[TMP_SECTION_SZ];
	phys_addr_t hyp_mem_base;
	unsigned long hyp_mem_size = 0;

	if (pkvm_init)
		goto switch_pgt;

	if (section_sz > TMP_SECTION_SZ) {
		pkvm_err("pkvm: no enough space to save sections[] array parameters!");
		ret = -ENOMEM;
		goto out;
	}

	/* kernel may use VMAP_STACK, which could make the parameter's vaddr
	 * not-valid after we switch new CR3 later, so copy parameter sections
	 * array from host space to pkvm space
	 */
	for (i = 0; i < section_sz; i++) {
		tmp_sections[i] = sections[i];
		if (sections[i].type == PKVM_RESERVED_MEMORY) {
			hyp_mem_base = sections[i].addr;
			hyp_mem_size = sections[i].size;
		}
	}
	if (hyp_mem_size == 0) {
		pkvm_err("pkvm: no pkvm reserve memory!");
		ret = -ENOTSUPP;
		goto out;
	}

	ret = divide_memory_pool(hyp_mem_base, hyp_mem_size);
	if (ret) {
		pkvm_err("pkvm: not reserve enough memory!");
		goto out;
	}

	ret = create_mmu_mapping(tmp_sections, section_sz);
	if (ret)
		goto out;

	/* TODO: setup host EPT page table */

	pkvm_init = true;

switch_pgt:
	/* switch mmu, TODO: switch EPT */
	vmcs_writel(HOST_CR3, pkvm_hyp->mmu->root_pa);
	pcpu->cr3 = pkvm_hyp->mmu->root_pa;

out:
	return ret;
}
