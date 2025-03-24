// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_vm_mgr: " fmt

#include <asm/gunyah.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "rsc_mgr.h"
#include "vm_mgr.h"

#define WRITE_TAG (1 << 0)
#define SHARE_TAG (1 << 1)

static inline struct gunyah_resource *
__first_resource(struct gunyah_vm_resource_ticket *ticket)
{
	return list_first_entry_or_null(&ticket->resources,
					struct gunyah_resource, list);
}

/*
 * Once the parcel is converted to paged, vm_mgr only tracks
 * the pages. The parcel needs to be reclaimed by the caller.
 */
int gunyah_vm_parcel_to_paged(struct gunyah_vm *ghvm,
			      struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			      u64 nr)
{
	struct gunyah_vm_parcel *vm_parcel;
	struct gunyah_rm_mem_entry *entry;
	struct gunyah_vm_binding *b;
	unsigned long i, tag = 0;
	struct folio *folio;
	pgoff_t off = 0;
	u64 size;
	int ret = 0;

	down_write(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, gfn);
	if (!b) {
		WARN_ON("No backing binding for the parcel being paged");
		ret = -ENOENT;
		goto unlock;
	}

	vm_parcel = b->vm_parcel;
	if (!vm_parcel) {
		WARN_ON("No parcel found");
		ret = -ENOENT;
		goto unlock;
	}

	if (parcel->n_acl_entries > 1)
		tag |= SHARE_TAG;
	if (parcel->acl_entries[0].perms & GUNYAH_RM_ACL_W)
		tag |= WRITE_TAG;

	for (i = 0; i < parcel->n_mem_entries; i++) {
		entry = &parcel->mem_entries[i];
		size = entry->size;
		folio = pfn_folio(PHYS_PFN(le64_to_cpu(entry->phys_addr)));
		while (size > 0) {
			ret = mtree_insert_range(&ghvm->mm, gfn + off,
						 gfn + off + folio_nr_pages(folio) - 1,
						 xa_tag_pointer(folio, tag),
						 GFP_KERNEL);
			if (ret) {
				WARN_ON(ret != -ENOMEM);
				gunyah_vm_mm_erase_range(ghvm, gfn, off - 1);
				goto unlock;
			}
			off += folio_nr_pages(folio);
			size -= folio_size(folio);
			folio = folio_next(folio);
		}
	}
	BUG_ON(off != nr);
	vm_parcel->start = 0;
	b->vm_parcel = NULL;

unlock:
	up_write(&ghvm->bindings_lock);
	return ret;
}

/**
 * gunyah_vm_mm_erase_range() - Erases a range of folios from ghvm's mm
 * @ghvm: gunyah vm
 * @gfn: start guest frame number
 * @nr: number of pages to erase
 *
 * Do not use this function unless rolling back gunyah_vm_parcel_to_paged.
 */
void gunyah_vm_mm_erase_range(struct gunyah_vm *ghvm, u64 gfn, u64 nr)
{
	struct folio *folio;
	u64 off = gfn;
	void *entry;

	while (off < gfn + nr) {
		entry = mtree_erase(&ghvm->mm, off);
		if (!entry)
			return;
		folio = xa_untag_pointer(entry);
		if (!folio)
			return;
		off += folio_nr_pages(folio);
	}
}

static inline u32 donate_flags(bool share)
{
	if (share)
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_SIBLING);
	else
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_PROTECTED);
}

static inline u32 reclaim_flags(bool share, bool sync)
{
	u32 flags = 0;

	if (share)
		flags |= FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					  GUNYAH_MEMEXTENT_DONATE_TO_SIBLING);
	else
		flags |= FIELD_PREP_CONST(
			GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
			GUNYAH_MEMEXTENT_DONATE_FROM_PROTECTED);

	if (!sync)
		flags |= GUNYAH_MEMEXTENT_OPTION_NOSYNC;

	return flags;
}

int gunyah_vm_provide_folio(struct gunyah_vm *ghvm, struct folio *folio,
			    u64 gfn, bool share, bool write)
{
	struct gunyah_resource *guest_extent, *host_extent, *addrspace;
	u32 map_flags = BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL);
	u64 extent_attrs, gpa = gunyah_gfn_to_gpa(gfn);
	phys_addr_t pa = PFN_PHYS(folio_pfn(folio));
	enum gunyah_pagetable_access access;
	size_t size = folio_size(folio);
	enum gunyah_error gunyah_error;
	unsigned long tag = 0;
	int ret, tmp;

	/* clang-format off */
	if (share) {
		guest_extent = __first_resource(&ghvm->guest_shared_extent_ticket);
		host_extent = __first_resource(&ghvm->host_shared_extent_ticket);
	} else {
		guest_extent = __first_resource(&ghvm->guest_private_extent_ticket);
		host_extent = __first_resource(&ghvm->host_private_extent_ticket);
	}
	/* clang-format on */
	addrspace = __first_resource(&ghvm->addrspace_ticket);

	if (!addrspace || !guest_extent || !host_extent)
		return -ENODEV;

	if (share) {
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_VMMIO);
		tag |= SHARE_TAG;
	} else {
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PRIVATE);
	}

	if (write)
		tag |= WRITE_TAG;

	ret = mtree_insert_range(&ghvm->mm, gfn,
				 gfn + folio_nr_pages(folio) - 1,
				 xa_tag_pointer(folio, tag), GFP_KERNEL);
	if (ret == -EEXIST)
		ret = -EAGAIN;
	if (ret)
		return ret;

	if (share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RW;
	else if (share && !write)
		access = GUNYAH_PAGETABLE_ACCESS_R;
	else if (!share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RWX;
	else /* !share && !write */
		access = GUNYAH_PAGETABLE_ACCESS_RX;

	ret = gunyah_rm_platform_pre_demand_page(ghvm->rm, ghvm->vmid, access,
						 folio);
	if (ret)
		goto reclaim_host;

	gunyah_error = gunyah_hypercall_memextent_donate(donate_flags(share),
							 host_extent->capid,
							 guest_extent->capid,
							 pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to donate memory for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto platform_release;
	}

	extent_attrs =
		FIELD_PREP_CONST(GUNYAH_MEMEXTENT_MAPPING_TYPE,
				 ARCH_GUNYAH_DEFAULT_MEMTYPE) |
		FIELD_PREP(GUNYAH_MEMEXTENT_MAPPING_USER_ACCESS, access) |
		FIELD_PREP(GUNYAH_MEMEXTENT_MAPPING_KERNEL_ACCESS, access);
	gunyah_error = gunyah_hypercall_addrspace_map(addrspace->capid,
						      guest_extent->capid, gpa,
						      extent_attrs, map_flags,
						      pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to map guest address 0x%016llx: %d\n", gpa,
		       gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto memextent_reclaim;
	}

	return 0;
memextent_reclaim:
	gunyah_error = gunyah_hypercall_memextent_donate(
		reclaim_flags(share, true), guest_extent->capid,
		host_extent->capid, pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK)
		pr_err("Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
platform_release:
	tmp = gunyah_rm_platform_reclaim_demand_page(ghvm->rm, ghvm->vmid,
						     access, folio);
	if (tmp) {
		pr_err("Platform failed to reclaim memory for guest address 0x%016llx: %d",
		       gpa, tmp);
		return ret;
	}
reclaim_host:
	gunyah_folio_host_reclaim(folio);
	mtree_erase(&ghvm->mm, gfn);
	return ret;
}

static int __gunyah_vm_reclaim_folio_locked(struct gunyah_vm *ghvm, void *entry,
					    u64 gfn, const bool sync)
{
	u32 map_flags = BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL);
	struct gunyah_resource *guest_extent, *host_extent, *addrspace;
	enum gunyah_pagetable_access access;
	enum gunyah_error gunyah_error;
	struct folio *folio;
	bool write, share;
	phys_addr_t pa;
	size_t size;
	int ret;

	addrspace = __first_resource(&ghvm->addrspace_ticket);
	if (!addrspace)
		return -ENODEV;

	share = !!(xa_pointer_tag(entry) & SHARE_TAG);
	write = !!(xa_pointer_tag(entry) & WRITE_TAG);
	folio = xa_untag_pointer(entry);

	if (!sync)
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_NOSYNC);

	/* clang-format off */
	if (share) {
		guest_extent = __first_resource(&ghvm->guest_shared_extent_ticket);
		host_extent = __first_resource(&ghvm->host_shared_extent_ticket);
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_VMMIO);
	} else {
		guest_extent = __first_resource(&ghvm->guest_private_extent_ticket);
		host_extent = __first_resource(&ghvm->host_private_extent_ticket);
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PRIVATE);
	}
	/* clang-format on */

	pa = PFN_PHYS(folio_pfn(folio));
	size = folio_size(folio);

	gunyah_error = gunyah_hypercall_addrspace_unmap(addrspace->capid,
							guest_extent->capid,
							gunyah_gfn_to_gpa(gfn),
							map_flags, pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err_ratelimited(
			"Failed to unmap guest address 0x%016llx: %d\n",
			gunyah_gfn_to_gpa(gfn), gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	gunyah_error = gunyah_hypercall_memextent_donate(
		reclaim_flags(share, sync), guest_extent->capid,
		host_extent->capid, pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err_ratelimited(
			"Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
			gunyah_gfn_to_gpa(gfn), gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	if (share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RW;
	else if (share && !write)
		access = GUNYAH_PAGETABLE_ACCESS_R;
	else if (!share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RWX;
	else /* !share && !write */
		access = GUNYAH_PAGETABLE_ACCESS_RX;

	ret = gunyah_rm_platform_reclaim_demand_page(ghvm->rm, ghvm->vmid,
						     access, folio);
	if (ret) {
		pr_err_ratelimited(
			"Platform failed to reclaim memory for guest address 0x%016llx: %d",
			gunyah_gfn_to_gpa(gfn), ret);
		goto err;
	}

	BUG_ON(mtree_erase(&ghvm->mm, gfn) != entry);

	unpin_user_page(folio_page(folio, 0));
	account_locked_vm(current->mm, 1, false);
	return 0;
err:
	return ret;
}

int gunyah_vm_reclaim_folio(struct gunyah_vm *ghvm, u64 gfn, struct folio *folio)
{
	void *entry;

	entry = mtree_load(&ghvm->mm, gfn);
	if (!entry)
		return 0;

	if (folio != xa_untag_pointer(entry))
		return -EAGAIN;

	return __gunyah_vm_reclaim_folio_locked(ghvm, entry, gfn, true);
}

int gunyah_vm_reclaim_range(struct gunyah_vm *ghvm, u64 gfn, u64 nr)
{
	unsigned long next = gfn, g;
	struct folio *folio;
	int ret, ret2 = 0;
	void *entry;
	bool sync;

	mt_for_each(&ghvm->mm, entry, next, gfn + nr) {
		folio = xa_untag_pointer(entry);
		g = next;
		sync = !mt_find_after(&ghvm->mm, &g, gfn + nr);

		g = next - folio_nr_pages(folio);
		folio_get(folio);
		folio_lock(folio);
		if (mtree_load(&ghvm->mm, g) == entry)
			ret = __gunyah_vm_reclaim_folio_locked(ghvm, entry, g, sync);
		else
			ret = -EAGAIN;
		folio_unlock(folio);
		folio_put(folio);
		if (ret && ret2 != -EAGAIN)
			ret2 = ret;
	}

	return ret2;
}

int gunyah_vm_binding_alloc(struct gunyah_vm *ghvm,
			    struct gunyah_userspace_memory_region *region,
			    bool lend)
{
	struct gunyah_vm_binding *binding;
	int ret = 0;

	if (!region->memory_size || !PAGE_ALIGNED(region->memory_size) ||
		!PAGE_ALIGNED(region->userspace_addr) ||
		!PAGE_ALIGNED(region->guest_phys_addr))
		return -EINVAL;

	if (overflows_type(region->guest_phys_addr + region->memory_size, u64))
		return -EOVERFLOW;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL_ACCOUNT);
	if (!binding) {
		return -ENOMEM;
	}

	binding->mem_type = VM_MEM_USER;
	binding->userspace_addr = region->userspace_addr;
	binding->vm_parcel = NULL;
	binding->guest_phys_addr = region->guest_phys_addr;
	binding->size = region->memory_size;
	binding->flags = region->flags;
	binding->label = region->label;

	if (lend) {
		binding->share_type = VM_MEM_LEND;
	} else {
		binding->share_type = VM_MEM_SHARE;
	}
	down_write(&ghvm->bindings_lock);
	ret = mtree_insert_range(&ghvm->bindings,
				 gunyah_gpa_to_gfn(binding->guest_phys_addr),
				 gunyah_gpa_to_gfn(binding->guest_phys_addr + region->memory_size - 1),
				 binding, GFP_KERNEL);

	if(ret != 0)
		kfree(binding);
	up_write(&ghvm->bindings_lock);

	return ret;
}

static int gunyah_gup_demand_page(struct gunyah_vm *ghvm, struct gunyah_vm_binding *b,
								u64 gpa, bool write)
{
	unsigned long gfn = gunyah_gpa_to_gfn(gpa);
	unsigned int gup_flags;
	u64 offset;
	int pinned, ret;
	struct page *page;
	struct folio *folio;

	if (write && !(b->flags & GUNYAH_MEM_ALLOW_WRITE))
		return -EPERM;
	gup_flags = FOLL_LONGTERM;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		gup_flags |= FOLL_WRITE;

	offset =  (gunyah_gfn_to_gpa(gfn) - b->guest_phys_addr);

	ret = account_locked_vm(current->mm, 1, true);
	if (ret)
		return ret;

	pinned = pin_user_pages_fast(b->userspace_addr + offset, 1,
					gup_flags, &page);

	if (pinned != 1) {
		ret = pinned;
		goto unlock_page;
	}

	folio = page_folio(page);

	if (!folio_test_swapbacked(folio)) {
		ret = -EIO;
		goto unpin_page;
	}

	folio_lock(folio);
	ret = gunyah_vm_provide_folio(ghvm, folio, gfn - folio_page_idx(folio, page),
				      !(b->share_type == VM_MEM_LEND),
				      !!(b->flags & GUNYAH_MEM_ALLOW_WRITE));
	folio_unlock(folio);
	if (ret) {
		if (ret != -EAGAIN)
			pr_err_ratelimited(
				"Failed to provide folio for guest addr: %016llx: %d\n",
				gpa, ret);
		goto unpin_page;
	}
	return ret;

unpin_page:
	unpin_user_page(page);
unlock_page:
	account_locked_vm(current->mm, 1, false);
	return ret;
}

int gunyah_demand_page(struct gunyah_vm *ghvm, u64 gpa, bool write)
{
	unsigned long gfn = gunyah_gpa_to_gfn(gpa);
	struct gunyah_vm_binding *b;
	int ret;

	down_read(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, gfn);
	if (!b) {
		ret = -ENOENT;
		goto unlock;
	}

	if (b->mem_type == VM_MEM_CMA) {
		dev_warn(ghvm->parent, "Demand paging of CMA mem not supported\n");
		ret = -EOPNOTSUPP;
	} else {
		ret = gunyah_gup_demand_page(ghvm, b, gpa, write);
	}

unlock:
	up_read(&ghvm->bindings_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_demand_page);

static int gunyah_gup_share_parcel(struct gunyah_vm *ghvm,
			struct gunyah_vm_parcel *vm_parcel,
			struct gunyah_vm_binding *b, u64 *gfn, u64 *nr)
{
	struct gunyah_rm_mem_parcel *parcel = &vm_parcel->parcel;
	struct page **pages;
	int pinned, ret;
	struct folio *folio;
	unsigned int gup_flags;
	unsigned long i, offset, entries, entry_size;

	offset = gunyah_gfn_to_gpa(*gfn) - b->guest_phys_addr;
	pages = kcalloc(*nr, sizeof(*pages), GFP_KERNEL_ACCOUNT);
	if (!pages)
		return -ENOMEM;

	gup_flags = FOLL_LONGTERM;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		gup_flags |= FOLL_WRITE;

	pinned = pin_user_pages_fast(b->userspace_addr + offset, *nr,
			gup_flags, pages);
	if (pinned < 0) {
		ret = pinned;
		goto free_pages;
	} else if (pinned != *nr) {
		ret = -EFAULT;
		goto unpin_pages;
	}

	ret = account_locked_vm(current->mm, pinned, true);
	if (ret)
		goto unpin_pages;

	/* overallocate & assume no large folios */
	parcel->mem_entries = kcalloc(pinned, sizeof(parcel->mem_entries[0]),
					GFP_KERNEL_ACCOUNT);
	if (!parcel->mem_entries) {
		ret = -ENOMEM;
		goto unaccount_pages;
	}
	folio = page_folio(pages[0]);
	*gfn -= folio_page_idx(folio, pages[0]);
	*nr = folio_nr_pages(folio);
	parcel->mem_entries[0].phys_addr = cpu_to_le64(PFN_PHYS(folio_pfn(folio)));
	entry_size = cpu_to_le64(folio_size(folio));

	for (i = 1, entries = 0; i < pinned; i++) {
		folio = page_folio(pages[i]);
		if (pages[i] == folio_page(folio, 0)) {
			if (page_to_pfn(pages[i - 1]) + 1 == page_to_pfn(pages[i])) {
				entry_size += cpu_to_le64(folio_size(folio));
				*nr += folio_nr_pages(folio);
			} else {
				parcel->mem_entries[entries].size = entry_size;
				entries++;
				parcel->mem_entries[entries].phys_addr = cpu_to_le64(PFN_PHYS(folio_pfn(folio)));
				entry_size = cpu_to_le64(folio_size(folio));
				*nr += folio_nr_pages(folio);
			}
		} else {
			unpin_user_page(pages[i]);
			account_locked_vm(current->mm, 1, false);
		}
	}
	parcel->mem_entries[entries].size = entry_size;
	parcel->n_mem_entries = entries + 1;
	ret = gunyah_rm_mem_share(ghvm->rm, parcel);
	if (ret)
		goto free_mem_entries;

	vm_parcel->start = *gfn;
	vm_parcel->pages = *nr;
	b->vm_parcel = vm_parcel;
	goto free_pages;

free_mem_entries:
	kfree(parcel->mem_entries);
	parcel->mem_entries = NULL;
	parcel->n_mem_entries = 0;
unaccount_pages:
	account_locked_vm(current->mm, pinned, false);
unpin_pages:
	unpin_user_pages(pages, pinned);
free_pages:
	kfree(pages);
	return ret;
}

int gunyah_share_parcel(struct gunyah_vm *ghvm, struct gunyah_vm_parcel *vm_parcel,
			     u64 *gfn, u64 *nr)
{
	struct gunyah_rm_mem_parcel *parcel = &vm_parcel->parcel;
	struct gunyah_vm_binding *b;
	bool lend;
	u16 vmid;
	int ret;

	if (!*nr)
		return -EINVAL;

	down_write(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, *gfn);
	if (!b) {
		ret = -ENOENT;
		goto unlock;
	}

	parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;
	if (b->share_type == VM_MEM_LEND) {
		parcel->n_acl_entries = 1;
		lend = true;
	} else {
		lend = false;
		parcel->n_acl_entries = 2;
		parcel->label = b->label;
	}
	parcel->acl_entries = kcalloc(parcel->n_acl_entries,
				      sizeof(*parcel->acl_entries), GFP_KERNEL);
	if (!parcel->acl_entries) {
		ret = -ENOMEM;
		goto unlock;
	}

	/* acl_entries[0].vmid will be this VM's vmid. We'll fill it when the
	 * VM is starting and we know the VM's vmid.
	 */
	parcel->acl_entries[0].vmid = cpu_to_le16(ghvm->vmid);
	if (b->flags & GUNYAH_MEM_ALLOW_READ)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_R;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_W;
	if (b->flags & GUNYAH_MEM_ALLOW_EXEC)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_X;

	if (!lend) {
		ret = gunyah_rm_get_vmid(ghvm->rm, &vmid);
		if (ret)
			goto free_acl;

		parcel->acl_entries[1].vmid = cpu_to_le16(vmid);
		/* Host assumed to have all these permissions. Gunyah will not
		* grant new permissions if host actually had less than RWX
		*/
		parcel->acl_entries[1].perms = GUNYAH_RM_ACL_R | GUNYAH_RM_ACL_W | GUNYAH_RM_ACL_X;
	}

	if (b->mem_type == VM_MEM_CMA) {
		ret = gunyah_cma_share_parcel(ghvm, vm_parcel, b, gfn, nr);
		if (ret) {
			dev_warn(ghvm->parent, "Failed to share CMA memory: %d\n", ret);
			goto free_acl;
		}
	} else {
		ret = gunyah_gup_share_parcel(ghvm, vm_parcel, b, gfn, nr);
		if (ret) {
			dev_warn(ghvm->parent, "Failed to share GUP memory: %d\n", ret);
			goto free_acl;
		}
	}
	goto unlock;

free_acl:
	kfree(parcel->acl_entries);
	parcel->acl_entries = NULL;
unlock:
	up_write(&ghvm->bindings_lock);
	return ret;
}

/*
 * This function will provide the number of bindings from
 * start_addr to end_addr.
 * Use ULONG_MAX as the end_addr to get all the bindings of the VM.
 */
static u32 gunyah_count_bindings(struct gunyah_vm *ghvm, u64 start_addr,
						u64 end_addr)
{
	struct gunyah_vm_binding *b;
	unsigned long addr = start_addr;
	u32 count = 0;

	down_read(&ghvm->bindings_lock);
	mt_for_each(&ghvm->bindings, b, addr, end_addr)
		count++;
	up_read(&ghvm->bindings_lock);

	return count;
}

static int gunyah_gup_reclaim_parcel(struct gunyah_vm *ghvm,
		struct gunyah_vm_parcel *vm_parcel, struct gunyah_vm_binding *b)
{
	struct gunyah_rm_mem_parcel *parcel = &vm_parcel->parcel;
	struct gunyah_rm_mem_entry *entry;
	struct folio *folio;
	pgoff_t i;
	int ret;

	if (parcel->mem_handle == GUNYAH_MEM_HANDLE_INVAL)
		return 0;

	ret = gunyah_rm_mem_reclaim(ghvm->rm, parcel);
	if (ret) {
		dev_err(ghvm->parent, "Failed to reclaim parcel: %d\n",
			ret);
		/* We can't reclaim the pages -- hold onto the pages
		 * forever because we don't know what state the memory
		 * is in
		 */
		return ret;
	}

	for (i = 0; i < parcel->n_mem_entries; i++) {
		entry = &parcel->mem_entries[i];

		folio = pfn_folio(PHYS_PFN(le64_to_cpu(entry->phys_addr)));

		if (folio_test_private(folio))
			gunyah_folio_host_reclaim(folio);

		unpin_user_page(folio_page(folio, 0));
		account_locked_vm(ghvm->mm_s, 1, false);
	}

	parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;
	kfree(parcel->mem_entries);
	kfree(parcel->acl_entries);
	vm_parcel->start = 0;
	vm_parcel->pages = 0;
	b->vm_parcel = NULL;
	return ret;
}

static int gunyah_reclaim_parcel(struct gunyah_vm *ghvm,
			    struct gunyah_vm_parcel *vm_parcel)
{
	struct gunyah_vm_binding *b;
	int ret;

	down_write(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, vm_parcel->start);
	if (!b) {
		ret = -ENOENT;
		goto unlock;
	}

	if (b->mem_type == VM_MEM_CMA)
		ret = gunyah_cma_reclaim_parcel(ghvm, vm_parcel, b);
	else
		ret = gunyah_gup_reclaim_parcel(ghvm, vm_parcel, b);

unlock:
	up_write(&ghvm->bindings_lock);
	return ret;
}

int gunyah_reclaim_parcels(struct gunyah_vm *ghvm, u64 start_gfn,
							u64 end_gfn)
{
	unsigned long gfn = start_gfn;
	struct gunyah_vm_binding *b;
	int ret, ret2 = 0;

	mt_for_each(&ghvm->bindings, b, gfn, end_gfn) {
		if (b->vm_parcel)
			ret = gunyah_reclaim_parcel(ghvm, b->vm_parcel);
		if (ret)
			ret2 = ret;
	}

	return ret2;
}

/*
 * gunyah_share_range_as_parcels() - Share all bindings as parcels from start_gfn to end_gfn
 * @ghvm - The gunyah vm
 * @start_gfn: Start guest page number
 * @end_gfn: Last guest page number
 * @parcels: Array of parcels allocated.
 *
 * Use ULONG_MAX as the end_gfn to share all the bindings of the VM
 * provided enough space for parcels is present.
 * Caller is responsible to free the parcels when parcels are done
 * being used.
 */
int gunyah_share_range_as_parcels(struct gunyah_vm *ghvm, u64 start_gfn,
				u64 end_gfn, struct gunyah_vm_parcel **parcels)
{
	struct gunyah_vm_binding *b;
	unsigned long gfn = start_gfn;
	u32 count = 0, n;
	int ret, ret_err;

	/* Find the number of parcels needed to be created within the requested range*/
	n = gunyah_count_bindings(ghvm, start_gfn, end_gfn);

	*parcels = kzalloc(sizeof(struct gunyah_vm_parcel) * n, GFP_KERNEL);
	if (!*parcels)
		return -ENOMEM;

	mt_for_each(&ghvm->bindings, b, gfn, end_gfn) {
		u64 parcel_start = b->guest_phys_addr >> PAGE_SHIFT;
		u64 parcel_pages = b->size >> PAGE_SHIFT;

		ret = gunyah_share_parcel(ghvm, &(*parcels)[count++], &parcel_start, &parcel_pages);
		if (ret) {
			dev_err(ghvm->parent, "Failed to share parcel of %llx: %d\n",
								parcel_start, ret);
			/* Let's roll back.*/
			while (count--) {
				if ((*parcels)[count].parcel.mem_handle !=
					GUNYAH_MEM_HANDLE_INVAL) {
					ret_err = gunyah_reclaim_parcel(ghvm, &(*parcels)[count]);
					if (ret_err)
						dev_err(ghvm->parent, "Failed to reclaim parcel: %d, memory will leak\n",
										ret_err);
				}
			}
			goto err;
		}
	}
	return ret;

err:
	kfree(*parcels);
	*parcels = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_share_range_as_parcels);

int gunyah_setup_demand_paging(struct gunyah_vm *ghvm, u64 start_gfn,
				u64 end_gfn)
{
	struct gunyah_rm_mem_entry *entries;
	unsigned long gfn = start_gfn;
	struct gunyah_vm_binding *b;
	u32 count = 0, i;
	int ret = 0;

	down_read(&ghvm->bindings_lock);
	mt_for_each(&ghvm->bindings, b, gfn, end_gfn)
		if (b->share_type == VM_MEM_LEND &&
			(b->guest_phys_addr != ghvm->fw.config.guest_phys_addr))
			count++;

	if (!count)
		goto out;

	entries = kcalloc(count, sizeof(*entries), GFP_KERNEL);
	if (!entries) {
		ret = -ENOMEM;
		goto out;
	}

	gfn = start_gfn;
	i = 0;
	mt_for_each(&ghvm->bindings, b, gfn, end_gfn) {
		if (b->share_type != VM_MEM_LEND ||
			(b->guest_phys_addr == ghvm->fw.config.guest_phys_addr))
			continue;
		entries[i].phys_addr = cpu_to_le64(b->guest_phys_addr);
		entries[i].size = cpu_to_le64(b->size);
		if (++i == count)
			break;
	}

	ret = gunyah_rm_vm_set_demand_paging(ghvm->rm, ghvm->vmid, i, entries);
	kfree(entries);
out:
	up_read(&ghvm->bindings_lock);
	return ret;
}
