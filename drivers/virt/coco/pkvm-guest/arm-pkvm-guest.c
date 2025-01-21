// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support for the hypercall interface exposed to protected guests by
 * pKVM.
 *
 * Author: Will Deacon <will@kernel.org>
 * Copyright (C) 2024 Google LLC
 */

#include <linux/arm-smccc.h>
#include <linux/array_size.h>
#include <linux/io.h>
#include <linux/mem_encrypt.h>
#include <linux/mem_relinquish.h>
#include <linux/mm.h>
#include <linux/pgtable.h>

#include <asm/hypervisor.h>

static size_t pkvm_granule;
static bool pkvm_func_range;

static int __arm_smccc_do(u32 func_id, phys_addr_t phys, int numgranules)
{
	while (numgranules--) {
		struct arm_smccc_res res;

		arm_smccc_1_1_invoke(func_id, phys, 0, 0, &res);
		if (res.a0 != SMCCC_RET_SUCCESS)
			return -EPERM;

		phys += pkvm_granule;
	}

	return 0;
}

static int __arm_smccc_do_range(u32 func_id, phys_addr_t phys, int numgranules)
{
	while (numgranules) {
		struct arm_smccc_res res;

		arm_smccc_1_1_invoke(func_id, phys, numgranules, 0, &res);
		if (res.a0 != SMCCC_RET_SUCCESS)
			return -EPERM;

		phys += pkvm_granule * res.a1;
		numgranules -= res.a1;
	}

	return 0;
}

/*
 * Apply func_id on the range [phys : phys + numpages * PAGE_SIZE)
 *
 * Use with cautious. Boundaries of the range will be aligned with pkvm_granule.
 * This might lead to overshooting when pkvm_granule > PAGE_SIZE.
 */
static int arm_smccc_do_range(u32 func_id, phys_addr_t phys, int numpages,
			      bool func_has_range)
{
	size_t size = numpages * PAGE_SIZE;
	int numgranules;

	if (!IS_ALIGNED(phys, pkvm_granule))
		return -EINVAL;

	numgranules = DIV_ROUND_UP(size, pkvm_granule);
	phys = ALIGN_DOWN(phys, pkvm_granule);

	if (func_has_range)
		return __arm_smccc_do_range(func_id, phys, numgranules);

	return __arm_smccc_do(func_id, phys, numgranules);
}

static int pkvm_set_memory_encrypted(unsigned long addr, int numpages)
{
	if (!IS_ALIGNED(numpages * PAGE_SIZE, pkvm_granule))
		return -EINVAL;

	return arm_smccc_do_range(ARM_SMCCC_VENDOR_HYP_KVM_MEM_UNSHARE_FUNC_ID,
				  virt_to_phys((void *)addr), numpages, pkvm_func_range);
}

static int pkvm_set_memory_decrypted(unsigned long addr, int numpages)
{
	if (!IS_ALIGNED(numpages * PAGE_SIZE, pkvm_granule))
		return -EINVAL;

	return arm_smccc_do_range(ARM_SMCCC_VENDOR_HYP_KVM_MEM_SHARE_FUNC_ID,
				  virt_to_phys((void *)addr), numpages, pkvm_func_range);
}

static const struct arm64_mem_crypt_ops pkvm_crypt_ops = {
	.encrypt	= pkvm_set_memory_encrypted,
	.decrypt	= pkvm_set_memory_decrypted,
};

static int mmio_guard_ioremap_hook(phys_addr_t phys, size_t size,
				   pgprot_t *prot)
{
	pteval_t protval = pgprot_val(*prot);
	u32 func_id = pkvm_func_range ?
		ARM_SMCCC_VENDOR_HYP_KVM_MMIO_RGUARD_MAP_FUNC_ID :
		ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_MAP_FUNC_ID;

	/*
	 * We only expect MMIO emulation for regions mapped with device
	 * attributes.
	 */
	if (protval != PROT_DEVICE_nGnRE && protval != PROT_DEVICE_nGnRnE)
		return 0;

	WARN_ON_ONCE(arm_smccc_do_range(func_id, phys, DIV_ROUND_UP(size, PAGE_SIZE),
					pkvm_func_range));

	return 0;
}

#ifdef CONFIG_MEMORY_RELINQUISH

static bool mem_relinquish_available;

bool page_relinquish_disallowed(void)
{
	return mem_relinquish_available && (pkvm_granule > PAGE_SIZE);
}
EXPORT_SYMBOL_GPL(page_relinquish_disallowed);

void page_relinquish(struct page *page)
{
	phys_addr_t phys, end;
	u32 func_id = ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID;

	if (!mem_relinquish_available)
		return;

	phys = page_to_phys(page);
	end = phys + PAGE_SIZE;

	while (phys < end) {
		struct arm_smccc_res res;

		arm_smccc_1_1_invoke(func_id, phys, 0, 0, &res);
		BUG_ON(res.a0 != SMCCC_RET_SUCCESS);

		phys += pkvm_granule;
	}
}
EXPORT_SYMBOL_GPL(page_relinquish);

#endif

void pkvm_init_hyp_services(void)
{
	struct arm_smccc_res res;

	if (!kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_HYP_MEMINFO))
		return;

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID,
			     0, 0, 0, &res);
	if ((long)res.a0 < 0)
		return;

	pkvm_granule = res.a0;
	pkvm_func_range = !!res.a1;

	if (kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MEM_SHARE) &&
	    kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MEM_UNSHARE))
	    arm64_mem_crypt_ops_register(&pkvm_crypt_ops);

	if (kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_MAP))
		arm64_ioremap_prot_hook_register(&mmio_guard_ioremap_hook);

#ifdef CONFIG_MEMORY_RELINQUISH
	if (kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MEM_RELINQUISH))
		mem_relinquish_available = true;
#endif
}
