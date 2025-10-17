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
phys_addr_t pkvm_virt_to_phys(void *virt);

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

/* Calculate the total pages for Scalable IOMMU */
static inline unsigned long pkvm_iommu_pages(int max_pasid, int nr_pasid_pdev,
					     int nr_pdev, int nr_iommu, int qidesc_sz,
					     int qidesc_status_sz, int num_cpus)
{
	unsigned long res = 0;

	/* PASID page table pages for each PASID capable pdev */
	res += ((max_pasid >> 6) + (max_pasid >> 15)) * nr_pasid_pdev;
	/* PASID page table pages (PASID dir + PASID table) for each normal pdev */
	res += 2 * nr_pdev;
	/*
	 * Context table page count is the minumal value of
	 * total pdev number and 256 bus * 2 (in scalable mode).
	 * Each pdev may require a context page if its bdf is
	 * discrete enough.
	 */
	res += min(256 * 2, nr_pasid_pdev + nr_pdev);
	/* Root pages for each IOMMU */
	res += nr_iommu;
	/* Desc and desc_status pages for each IOMMU */
	res += nr_iommu * ((1 << get_order(qidesc_sz)) + (1 << get_order(qidesc_status_sz)));
	/*
	 * Reserve more IQ descriptor page. The size is calculated according to
	 * the IOMMU QI descriptor size(excludes the QI descriptor status as
	 * this is not needed to bunch requests) and the CPU number. Each CPU can
	 * have its own reserved QI descriptor page so that multiple CPUs can
	 * bunch the QI requests at the same time.
	 */
	res += num_cpus * (1 << get_order(qidesc_sz));

	return res;
}

/*
 * Calculate the total pages for shadow IOMMU page tables for the host's
 * devices used with Legacy IOMMU. Similarly to the calculation for shadow EPT,
 * we assume that there is no shared memory between devices using different
 * page tables.
 *
 * TODO: do not reserve these pages if legacy mode is not used by pKVM, i.e.
 * if all the IOMMUs have scalable mode capability.
 */
static inline unsigned long pkvm_host_shadow_iommu_pgtable_pages(int nr_pdev)
{
	unsigned long pgtable_pages = __pkvm_pgtable_total_pages();
	unsigned long res;

	res = pgtable_pages;

	/*
	 * Similarly to shadow VMs (see the comment in
	 * pkvm_shadow_ept_pgtable_pages()), each device may require
	 * its own level2:level5 page table pages.
	 */
	res += __pkvm_pgtable_max_pages(pgtable_pages) * (nr_pdev - 1);

	return res;
}


u64 hyp_total_reserve_pages(void);

static inline bool pkvm_is_protected_vm(struct kvm *kvm)
{
	return kvm->arch.vm_type == KVM_X86_PKVM_PROTECTED_VM;
}

static inline bool pkvm_is_protected_vcpu(struct kvm_vcpu *vcpu)
{
	return pkvm_is_protected_vm(vcpu->kvm);
}

static inline size_t pkvm_guest_initial_fpstate_size(struct kvm *kvm)
{
	/*
	 * The pkvm hypervisor requires to have at least the size of struct
	 * fpstate for both pVM (to switch FPU and emulate XFD MSR) and npVM
	 * (to emulate XFD MSR only).
	 */
	size_t size = ALIGN(offsetof(struct fpstate, regs), 64);

	/*
	 * The pkvm hypervisor switches the FPU registers for pVM thus the size
	 * should be extended with fpu_user_cfg.default_size to satisfy the
	 * default features (w/o dynamic features).
	 */
	if (pkvm_is_protected_vm(kvm))
		size += fpu_user_cfg.default_size;

	return PAGE_ALIGN(size);
}

int pkvm_vm_ioctl_enable_cap(struct kvm *kvm, struct kvm_enable_cap *cap);
void pkvm_create_vm_debugfs(struct kvm *kvm);
int kvm_share_hyp(void *from, void *to);
void kvm_unshare_hyp(void *from, void *to);
#else
static inline void kvm_hyp_reserve(void) {}
static inline bool pkvm_is_protected_vm(struct kvm *kvm) { return false; }
static inline bool pkvm_is_protected_vcpu(struct kvm_vcpu *vcpu) { return false; }
static inline int pkvm_vm_ioctl_enable_cap(struct kvm *kvm, struct kvm_enable_cap *cap)
{ return -EINVAL; }
static inline void pkvm_create_vm_debugfs(struct kvm *kvm) {}
#endif

enum pkvm_fn {
	__pkvm__enable_virtualization_cpu,
	__pkvm__disable_virtualization_cpu,
	__pkvm__check_processor_compatibility,
	__pkvm__vm_init,
	__pkvm__vm_finalize,
	__pkvm__vm_destroy,
	__pkvm__vm_mmu_map,
	__pkvm__vm_mmu_unmap,
	__pkvm__vm_mmu_age,
	__pkvm__vcpu_create,
	__pkvm__vcpu_free,
	__pkvm__vcpu_reset,
	__pkvm__vcpu_load,
	__pkvm__vcpu_put,
	__pkvm__vcpu_run,
	__pkvm__vcpu_after_set_cpuid,
	__pkvm__get_segment_base,
	__pkvm__get_segment,
	__pkvm__set_segment,
	__pkvm__set_cr0,
	__pkvm__set_cr4,
	__pkvm__set_msr,
	__pkvm__get_msr,
	__pkvm__set_efer,
	__pkvm__get_idt,
	__pkvm__set_idt,
	__pkvm__get_gdt,
	__pkvm__set_gdt,
	__pkvm__set_dr7,
	__pkvm__get_rflags,
	__pkvm__set_rflags,
	__pkvm__flush_tlb_all,
	__pkvm__flush_tlb_current,
	__pkvm__flush_tlb_gva,
	__pkvm__flush_tlb_guest,
	__pkvm__set_interrupt_shadow,
	__pkvm__get_interrupt_shadow,
	__pkvm__complete_emulated_msr,
	__pkvm__interrupt_allowed,
	__pkvm__nmi_allowed,
	__pkvm__inject_irq,
	__pkvm__inject_nmi,
	__pkvm__inject_exception,
	__pkvm__cancel_injection,
	__pkvm__get_nmi_mask,
	__pkvm__set_nmi_mask,
	__pkvm__enable_nmi_window,
	__pkvm__enable_irq_window,
	__pkvm__update_cr8_intercept,
	__pkvm__set_virtual_apic_mode,
	__pkvm__refresh_apicv_exec_ctrl,
	__pkvm__load_eoi_exitmap,
	__pkvm__hwapic_irr_update,
	__pkvm__hwapic_isr_update,
	__pkvm__write_tsc_offset,
	__pkvm__write_tsc_multiplier,
	__pkvm__post_set_cr3,
	__pkvm__load_mmu_pgd,
	__pkvm__setup_mce,
	__pkvm__cache_reg,
	__pkvm__update_cpuid_runtime,
	__pkvm__update_exception_bitmap,
	__pkvm__vcpu_add_fpstate,
	__pkvm__host_share_hyp,
	__pkvm__host_unshare_hyp,
};

#define HOST_HANDLE_EXIT		0
#define HOST_RESET_MMU			1
#define HOST_INIT_MMU			2
#define HOST_HANDLE_GUESTDBG_SINGLESTEP	3

union pkvm_pv_param {
	struct kvm_segment seg;
	struct msr_data msr;
	struct desc_ptr desc;
	u64 eoi_exit_bitmap[4];
} __aligned(PAGE_SIZE);

#endif
