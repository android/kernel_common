// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2023 Intel Corporation. */
#include <asm/pci_x86.h>
#include <asm/pkvm_spinlock.h>
#include <pkvm.h>

#include "io.h"
#include "io_emulate.h"
#include "mmu.h"
#include "ptdev.h"
#include "pci.h"

static union pci_cfg_addr_reg host_vpci_cfg_addr;
static pkvm_spinlock_t pci_cfg_lock = __PKVM_SPINLOCK_UNLOCKED;
static pkvm_spinlock_t host_vpci_cfg_lock = __PKVM_SPINLOCK_UNLOCKED;

static int pci_cfg_space_read(union pci_cfg_addr_reg *cfg_addr,
	u32 offset, int size, unsigned long *value)
{
	pkvm_spin_lock(&pci_cfg_lock);

	pkvm_pio_write(PCI_CFG_ADDR, 4, cfg_addr->value);
	pkvm_pio_read(PCI_CFG_DATA + offset, size, value);

	pkvm_spin_unlock(&pci_cfg_lock);

	return 0;
}

static int pci_cfg_space_write(union pci_cfg_addr_reg *cfg_addr,
	u32 offset, int size, unsigned long value)
{
	pkvm_spin_lock(&pci_cfg_lock);

	pkvm_pio_write(PCI_CFG_ADDR, 4, cfg_addr->value);
	pkvm_pio_write(PCI_CFG_DATA + offset, size, value);

	pkvm_spin_unlock(&pci_cfg_lock);

	return 0;
}

static int pci_mmcfg_read(u64 address, int size, unsigned long *value)
{
	pkvm_mmio_read(address, size, value);
	return 0;
}

static int pci_mmcfg_write(u64 address, int size, unsigned long value)
{
	pkvm_mmio_write(address, size, value);
	return 0;
}

unsigned long pkvm_pci_cfg_space_read(u32 bdf, u32 offset, int size)
{
	union pci_cfg_addr_reg reg;
	unsigned long value = 0;

	reg.enable = 1;
	reg.bdf = bdf;
	reg.reg = offset & (~0x3);

	pci_cfg_space_read(&reg, offset & 0x3, size, &value);

	return value;
}

void pkvm_pci_cfg_space_write(u32 bdf, u32 offset, int size, unsigned long value)
{
	union pci_cfg_addr_reg reg;

	reg.enable = 1;
	reg.bdf = bdf;
	reg.reg = offset & (~0x3);

	pci_cfg_space_write(&reg, offset & 0x3, size, value);
}

static bool host_vpci_cfg_data_allow_write(struct pkvm_ptdev *ptdev, u64 offset, int size, u32 value)
{
	int index;

	if (!ptdev_attached_to_vm(ptdev))
		return true;

	if (offset >= 0x10 && offset < 0x28) {
		index = (offset-0x10) >> 2;
		/* Allow only aligned BAR write with the cached value*/
		return (offset & 0x3) == 0 && size == 4 && value == ptdev->bars[index];
	}

	return true;
}

static int host_vpci_cfg_addr_read(struct kvm_vcpu *vcpu, struct pkvm_pio_req *req)
{
	u32 value = host_vpci_cfg_addr.value;
	int ret = 0;

	pkvm_spin_lock(&host_vpci_cfg_lock);

	switch (req->size) {
	case 1:
		*(u8 *)req->value = (u8)value;
		break;
	case 2:
		*(u16 *)req->value = (u16)value;
		break;
	case 4:
		*(u32 *)req->value = value;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pkvm_spin_unlock(&host_vpci_cfg_lock);

	return ret;
}

static int host_vpci_cfg_addr_write(struct kvm_vcpu *vcpu, struct pkvm_pio_req *req)
{
	u32 *value = &host_vpci_cfg_addr.value;
	int ret = 0;

	pkvm_spin_lock(&host_vpci_cfg_lock);

	switch (req->size) {
	case 1:
		*(u8 *)value = (u8)*req->value;
		break;
	case 2:
		*(u16 *)value = (u16)*req->value;
		break;
	case 4:
		*value = (u32)*req->value;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pkvm_spin_unlock(&host_vpci_cfg_lock);

	return ret;
}

static int host_vpci_cfg_data_audit_write(struct pkvm_pio_req *req)
{
	struct pkvm_ptdev *ptdev;
	u64 offset = host_vpci_cfg_addr.reg;
	u32 bdf = host_vpci_cfg_addr.bdf;
	int ret;

	ptdev = pkvm_get_ptdev(bdf, 0);

	if (ptdev) {
		pkvm_spin_lock(&ptdev->lock);
		if (!host_vpci_cfg_data_allow_write(ptdev, offset + req->port - PCI_CFG_DATA,
			req->size, *req->value)) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = pci_cfg_space_write(&host_vpci_cfg_addr, req->port - PCI_CFG_DATA, req->size, *req->value);

out:
	if (ptdev) {
		pkvm_spin_unlock(&ptdev->lock);
		pkvm_put_ptdev(ptdev);
	}

	return ret;
}

static int host_vpci_cfg_data_read(struct kvm_vcpu *vcpu, struct pkvm_pio_req *req)
{
	int ret;

	pkvm_spin_lock(&host_vpci_cfg_lock);

	if (host_vpci_cfg_addr.enable)
		ret = pci_cfg_space_read(&host_vpci_cfg_addr, req->port - PCI_CFG_DATA, req->size, req->value);
	else
		ret = -EINVAL;

	pkvm_spin_unlock(&host_vpci_cfg_lock);

	return ret;
}

static int host_vpci_cfg_data_write(struct kvm_vcpu *vcpu, struct pkvm_pio_req *req)
{
	int ret;

	pkvm_spin_lock(&host_vpci_cfg_lock);

	if (host_vpci_cfg_addr.enable)
		ret = host_vpci_cfg_data_audit_write(req);
	else
		ret = -EINVAL;

	pkvm_spin_unlock(&host_vpci_cfg_lock);

	return ret;
}

static int host_vpci_mmcfg_get_bdf_offset(u64 address, u32 *bdf, u64 *offset)
{
	int i;
	struct pkvm_pci_info *pci_info;
	struct pci_mmcfg_region *region;

	pci_info = &pkvm_hyp->host_vm.pci_info;
	for (i = 0; i < pci_info->mmcfg_table_size; i++) {
		region = &pci_info->mmcfg_table[i];
		if (address >= region->res.start && address <= region->res.end) {
			*bdf = (address - region->address) >> 12;
			*offset = address & 0xfff;
			return 0;
		}
	}

	return -EINVAL;
}

static int host_vpci_mmcfg_read(struct kvm_vcpu *vcpu, struct pkvm_mmio_req *req)
{
	u64 address = (u64)host_mmio2hva(req->address);

	return pci_mmcfg_read(address, req->size, req->value);
}

static int host_vpci_mmcfg_write(struct kvm_vcpu *vcpu, struct pkvm_mmio_req *req)
{
	struct pkvm_ptdev *ptdev;
	u64 offset, address = (u64)host_mmio2hva(req->address);
	u32 bdf;
	int ret;

	if (host_vpci_mmcfg_get_bdf_offset(req->address, &bdf, &offset))
		return -EINVAL;

	ptdev = pkvm_get_ptdev(bdf, 0);

	if (ptdev) {
		pkvm_spin_lock(&ptdev->lock);
		if (!host_vpci_cfg_data_allow_write(ptdev, offset, req->size, *req->value)) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = pci_mmcfg_write(address, req->size, *req->value);

out:
	if (ptdev) {
		pkvm_spin_unlock(&ptdev->lock);
		pkvm_put_ptdev(ptdev);
	}

	return ret;
}

int init_pci(struct pkvm_hyp *pkvm)
{
	int ret;

	ret = register_host_pio_handler(&pkvm->host_vm,
		PCI_CFG_ADDR, IO_SIZE_4, host_vpci_cfg_addr_read, host_vpci_cfg_addr_write);
	if (ret)
		goto out;

	/*
	 * Kernel access the PCI config space data port in an unaligned way. So here we
	 * treat the data port as four consecutive ports and register four handlers for it.
	 * All registered ports and access width below are valid.
	 */
	ret = register_host_pio_handler(&pkvm->host_vm,
		PCI_CFG_DATA, IO_SIZE_FULL, host_vpci_cfg_data_read, host_vpci_cfg_data_write);
	if (ret)
		goto out;

	ret = register_host_pio_handler(&pkvm->host_vm,
		PCI_CFG_DATA + 1, IO_SIZE_1, host_vpci_cfg_data_read, host_vpci_cfg_data_write);
	if (ret)
		goto out;

	ret = register_host_pio_handler(&pkvm->host_vm,
		PCI_CFG_DATA + 2, IO_SIZE_1 | IO_SIZE_2, host_vpci_cfg_data_read, host_vpci_cfg_data_write);
	if (ret)
		goto out;

	ret = register_host_pio_handler(&pkvm->host_vm,
		PCI_CFG_DATA + 3, IO_SIZE_1, host_vpci_cfg_data_read, host_vpci_cfg_data_write);
	if (ret)
		goto out;

	return 0;

out:
	pkvm_err("pkvm: init pci failed");
	return ret;
}

static int pkvm_mmu_map_mmcfg_region(struct pkvm_pci_info *pci_info)
{
	struct pci_mmcfg_region *region;
	int i, ret;
	u64 start, end;

	for (i = 0; i < pci_info->mmcfg_table_size; i++) {
		region = &pci_info->mmcfg_table[i];
		start = region->res.start;
		end = region->res.end;
		ret = pkvm_mmu_map((u64)host_mmio2hva(start), start,
			end - start + 1, 0, (u64)pgprot_val(PAGE_KERNEL_IO));
		if (ret)
			return ret;
	}

	return 0;
}

int init_finalize_pci(struct pkvm_pci_info *pci_info)
{
	struct pci_mmcfg_region *region;
	unsigned long start, end;
	int ret, i;

	ret = pkvm_mmu_map_mmcfg_region(pci_info);
	if (ret)
		return ret;

	for (i = 0; i < pci_info->mmcfg_table_size; i++) {
		region = &pci_info->mmcfg_table[i];
		start = region->res.start;
		end = region->res.end;

		ret = register_host_mmio_handler(start, end,
			host_vpci_mmcfg_read, host_vpci_mmcfg_write);
		if (ret)
			return ret;
	}

	return 0;
}
