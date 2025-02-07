// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/pkvm.h>

#include <kvm/arm_hypercalls.h>
#include <kvm/device.h>

struct pkvm_device *registered_devices;
unsigned long registered_devices_nr;

/*
 * This lock protects all devices in registered_devices when ctxt changes,
 * this is overlocking and can be improved. However, the device context
 * only changes at boot time and at teardown and in theory there shouldn't
 * be congestion on that path.
 * All changes/checks to MMIO state or IOMMU must be atomic with the ctxt
 * of the device.
 */
static DEFINE_HYP_SPINLOCK(device_spinlock);

int pkvm_init_devices(void)
{
	size_t dev_sz;
	int ret;

	registered_devices = kern_hyp_va(registered_devices);
	dev_sz = PAGE_ALIGN(size_mul(sizeof(struct pkvm_device),
				     registered_devices_nr));

	ret = __pkvm_host_donate_hyp(hyp_virt_to_phys(registered_devices) >> PAGE_SHIFT,
				     dev_sz >> PAGE_SHIFT);
	if (ret)
		registered_devices_nr = 0;
	return ret;
}

/* return device from a resource, addr and size must match. */
static struct pkvm_device *pkvm_get_device(u64 addr, size_t size)
{
	struct pkvm_device *dev;
	struct pkvm_dev_resource *res;
	int i, j;

	for (i = 0 ; i < registered_devices_nr ; ++i) {
		dev = &registered_devices[i];
		for (j = 0 ; j < dev->nr_resources; ++j) {
			res = &dev->resources[j];
			if ((addr == res->base) && (size == res->size))
				return dev;
		}
	}

	return NULL;
}

static struct pkvm_device *pkvm_get_device_by_addr(u64 addr)
{
	struct pkvm_device *dev;
	struct pkvm_dev_resource *res;
	int i, j;

	for (i = 0 ; i < registered_devices_nr ; ++i) {
		dev = &registered_devices[i];
		for (j = 0 ; j < dev->nr_resources; ++j) {
			res = &dev->resources[j];
			if ((addr >= res->base) && (addr < res->base + res->size))
				return dev;
		}
	}

	return NULL;
}

/*
 * Devices assigned to guest has to transition first to hypervisor,
 * this guarantees that there is a point of time that the device is
 * neither accessible from the host or the guest, so the hypervisor
 * can reset it and block it's IOOMU.
 * The host will donate the whole device first to the hypervisor
 * before the guest touches or requests any part of the device
 * and upon the first request or access the hypervisor will ensure
 * that the device is fully donated first.
 */
int pkvm_device_hyp_assign_mmio(u64 pfn, u64 nr_pages)
{
	struct pkvm_device *dev;
	int ret;
	size_t size = nr_pages << PAGE_SHIFT;
	u64 phys = pfn << PAGE_SHIFT;

	dev = pkvm_get_device(phys, size);
	if (!dev)
		return -ENODEV;

	hyp_spin_lock(&device_spinlock);
	/* A VM already have this device, no take backs. */
	if (dev->ctxt) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = ___pkvm_host_donate_hyp_prot(pfn, nr_pages, true, PAGE_HYP_DEVICE);
	/* Hyp have device mapping, while host may have issue cacheable writes.*/
	if (!ret)
		kvm_flush_dcache_to_poc(__hyp_va(phys), PAGE_SIZE);

out_unlock:
	hyp_spin_unlock(&device_spinlock);
	return ret;
}

/*
 * Reclaim of MMIO can happen in two cases:
 * - VM is dying, in that case MMIO would be eagerly reclaimed to the host
 *   from VM teardown context without host intervention.
 * - The VM was not launched or died before claiming the device, and it's is
 *   still considered as host device, but the MMIO was already donated to
 *   the hypervisor preparing for the VM to access it, in that case the host
 *   will use this function from an HVC to reclaim the MMIO from KVM/VFIO
 *   file release context or incase of failure at initialization.
 */
int pkvm_device_reclaim_mmio(u64 pfn, u64 nr_pages)
{
	struct pkvm_device *dev;
	int ret;
	size_t size = nr_pages << PAGE_SHIFT;
	u64 phys = pfn << PAGE_SHIFT;

	dev = pkvm_get_device(phys, size);
	if (!dev)
		return -ENODEV;

	hyp_spin_lock(&device_spinlock);
	if (dev->ctxt) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = __pkvm_hyp_donate_host(pfn, nr_pages);

out_unlock:
	hyp_spin_unlock(&device_spinlock);
	return ret;
}

static int __pkvm_device_assign(struct pkvm_device *dev, struct pkvm_hyp_vm *vm)
{
	int i;
	struct pkvm_dev_resource *res;
	int ret;

	hyp_assert_lock_held(&device_spinlock);

	for (i = 0 ; i < dev->nr_resources; ++i) {
		res = &dev->resources[i];
		ret = hyp_check_range_owned(res->base, res->size);
		if (ret)
			return ret;
	}

	dev->ctxt = vm;
	return 0;
}

/*
 * Atomically check that all the group is assigned to the hypervisor
 * and tag the devices in the group as owned by the VM.
 * This can't race with reclaim as it's protected by device_spinlock
 */
static int __pkvm_group_assign(u32 group_id, struct pkvm_hyp_vm *vm)
{
	int i;
	int ret = 0;

	hyp_assert_lock_held(&device_spinlock);

	for (i = 0 ; i < registered_devices_nr ; ++i) {
		struct pkvm_device *dev = &registered_devices[i];

		if (dev->group_id != group_id)
			continue;
		if (dev->ctxt) {
			ret = -EPERM;
			break;
		}
		ret = __pkvm_device_assign(dev, vm);
		if (ret)
			break;
	}

	if (ret) {
		while (i--) {
			struct pkvm_device *dev = &registered_devices[i];

			if (dev->group_id == group_id)
				dev->ctxt = NULL;
		}
	}
	return ret;
}


int pkvm_host_map_guest_mmio(struct pkvm_hyp_vcpu *hyp_vcpu, u64 pfn, u64 gfn)
{
	int ret = 0;
	struct pkvm_device *dev = pkvm_get_device_by_addr(hyp_pfn_to_phys(pfn));
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	if (!dev)
		return -ENODEV;

	hyp_spin_lock(&device_spinlock);

	if (dev->ctxt == NULL) {
		/*
		 * First time the device is assigned to a guest, make sure the whole
		 * group is assigned to the hypervisor.
		 */
		ret = __pkvm_group_assign(dev->group_id, vm);
	} else if (dev->ctxt != vm) {
		ret = -EBUSY;
	}

	if (ret)
		goto out_ret;

	ret = __pkvm_install_guest_mmio(hyp_vcpu, pfn, gfn);

out_ret:
	hyp_spin_unlock(&device_spinlock);
	return ret;
}

static int pkvm_get_device_pa(struct pkvm_hyp_vcpu *hyp_vcpu, u64 ipa, u64 *pa, u64 *exit_code)
{
	struct kvm_hyp_req *req;
	int ret;
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	ret = __pkvm_guest_get_valid_phys_page(vm, pa, ipa);
	if (ret == -ENOENT) {
		/* Page not mapped, create a request*/
		req = pkvm_hyp_req_reserve(hyp_vcpu, KVM_HYP_REQ_TYPE_MAP);
		if (!req)
			return -ENOMEM;

		req->map.guest_ipa = ipa;
		req->map.size = PAGE_SIZE;
		*exit_code = ARM_EXCEPTION_HYP_REQ;
		/* Repeat next time. */
		write_sysreg_el2(read_sysreg_el2(SYS_ELR) - 4, SYS_ELR);
	}

	return ret;
}

bool pkvm_device_request_mmio(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code)
{
	int i, j, ret;
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	struct pkvm_hyp_vm *vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	u64 ipa = smccc_get_arg1(vcpu);
	u64 token;

	/* arg2 and arg3 reserved for future use. */
	if (smccc_get_arg2(vcpu) || smccc_get_arg3(vcpu) || !PAGE_ALIGNED(ipa))
		goto out_inval;

	ret = pkvm_get_device_pa(hyp_vcpu, ipa, &token, exit_code);
	if (ret == -ENOENT)
		return false;
	else if (ret)
		goto out_inval;

	hyp_spin_lock(&device_spinlock);
	for (i = 0 ; i < registered_devices_nr ; ++i) {
		struct pkvm_device *dev = &registered_devices[i];

		if (dev->ctxt != vm)
			continue;

		for (j = 0 ; j < dev->nr_resources; ++j) {
			struct pkvm_dev_resource *res = &dev->resources[j];

			if ((token >= res->base) && (token + PAGE_SIZE <= res->base + res->size)) {
				smccc_set_retval(vcpu, SMCCC_RET_SUCCESS, token, 0, 0);
				goto out_ret;
			}
		}
	}

	smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
out_ret:
	hyp_spin_unlock(&device_spinlock);
	return true;
out_inval:
	smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
	return true;
}
