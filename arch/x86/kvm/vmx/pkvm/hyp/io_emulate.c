// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2023 Intel Corporation. */
#include <pkvm.h>
#include "ept.h"
#include "io.h"
#include "io_emulate.h"

struct pkvm_pio_emul_table host_pio_emul_table;
struct pkvm_mmio_emul_table host_mmio_emul_table;

static int pkvm_pio_default_in(struct kvm_vcpu *vcpu, struct pkvm_pio_req *req)
{
	pkvm_pio_read(req->port, req->size, req->value);
	return 0;
}

static int pkvm_pio_default_out(struct kvm_vcpu *vcpu, struct pkvm_pio_req *req)
{
	pkvm_pio_write(req->port, req->size, *req->value);
	return 0;
}

struct pkvm_pio_handler default_pio_handler = {
	.read = pkvm_pio_default_in,
	.write = pkvm_pio_default_out
};

/*
 * Not thread safe and should hold a lock if called concurrently.
 */
int register_host_pio_handler(struct pkvm_host_vm *host_vm, unsigned int port,
	unsigned int size_mask, pio_handler_t read, pio_handler_t write)
{
	struct pkvm_pio_emul_table *table;
	struct pkvm_pio_handler *handler;
	unsigned long index;
	u8 bit;

	table = &host_pio_emul_table;
	index = find_first_zero_bit(table->bitmap, PKVM_MAX_PIO_EMUL_NUM);
	if (index >= PKVM_MAX_PIO_EMUL_NUM)
		return -ENOSPC;

	__set_bit(index, table->bitmap);

	handler = &table->table[index];
	handler->port = port;
	handler->size_mask = size_mask;
	handler->read = read;
	handler->write = write;

	index = port >> 3U;
	bit = (u8)(1U << (port & 0x7U));
	host_vm->io_bitmap[index] |= bit;

	return 0;
}

static bool pio_access_valid(int size)
{
	return size == IO_SIZE_1 || size == IO_SIZE_2 || size == IO_SIZE_4;
}

static struct pkvm_pio_handler *get_pio_handler(struct pkvm_pio_emul_table *table,
	struct pkvm_pio_req *req)
{
	struct pkvm_pio_handler *handler;
	unsigned long index;
	/*
	 * Port I/O access is expected to only based on their address and have a
	 * fixed access width. Note that they might overlap, for example PCI config
	 * space addr port 0xcf8 and ACPI reset port 0xcf9. So match the handler
	 * strictly based on their base address and access width here.
	 *
	 * There are two special situations to consider. One case is that the base
	 * address matches but the access width differs, this is regarded as an
	 * invalid access and thus return a NULL handler. Another case is no base
	 * address matches. This is due to an overlapped I/O access that triggered
	 * the IO VM exit, but we are not intended to handle the base address. So
	 * in this case choose the default handler to do plain pio.
	 */
	for_each_set_bit(index, table->bitmap, PKVM_MAX_PIO_EMUL_NUM) {
		handler = &table->table[index];
		if (req->port == handler->port) {
			if (pio_access_valid(req->size) && (req->size & handler->size_mask))
				return handler;

			pkvm_err("pkvm: I/O port 0x%x mismatched access witdth %d",
				req->port, req->size);
			return NULL;
		}
	}

	return &default_pio_handler;
}

static int emulate_host_pio(struct kvm_vcpu *vcpu, struct pkvm_pio_req *req)
{
	struct pkvm_pio_emul_table *table;
	struct pkvm_pio_handler *handler;
	int ret = 0;

	table = &host_pio_emul_table;
	handler = get_pio_handler(table, req);
	if (!handler)
		return -EINVAL;

	if (req->direction == PKVM_IO_READ && handler->read)
		ret = handler->read(vcpu, req);
	else if (req->direction == PKVM_IO_WRITE && handler->write)
		ret = handler->write(vcpu, req);

	return ret;
}

int handle_host_pio(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long exit_qual;
	struct pkvm_pio_req req;
	int string;

	exit_qual = vmx->exit_qualification;

	string = (exit_qual & 16) != 0;
	if (string) {
		pkvm_err("pkvm: unsupported string instruction\n");
		return -EINVAL;
	}

	req.port = exit_qual >> 16;
	req.size = (exit_qual & 7) + 1;
	req.value = &vcpu->arch.regs[VCPU_REGS_RAX];
	req.direction = (exit_qual & 8) == 0;

	pkvm_dbg("pkvm: host %s I/O port 0x%x width %d value %lx", req.direction ?
		"write" : "read", req.port, req.size, *req.value);

	return emulate_host_pio(vcpu, &req);
}

static int pkvm_mmio_default_read(struct kvm_vcpu *vcpu, struct pkvm_mmio_req *req)
{
	pkvm_mmio_read((u64)host_mmio2hva(req->address), req->size, req->value);
	return 0;
}

static int pkvm_mmio_default_write(struct kvm_vcpu *vcpu, struct pkvm_mmio_req *req)
{
	pkvm_mmio_write((u64)host_mmio2hva(req->address), req->size, *req->value);
	return 0;
}

struct pkvm_mmio_handler default_mmio_handler = {
	.read = pkvm_mmio_default_read,
	.write = pkvm_mmio_default_write
};

static struct pkvm_mmio_handler *emul_mmio_lookup(struct pkvm_mmio_emul_table *table,
	unsigned long start, unsigned long end)
{
	struct pkvm_mmio_handler *handler;
	unsigned long index;

	for_each_set_bit(index, table->bitmap, PKVM_MAX_MMIO_EMUL_NUM) {
		handler = &table->table[index];
		if (start <= handler->end && handler->start <= end)
			return handler;
	}

	return NULL;
}

/*
 * Not thread safe and should hold a lock if called concurrently.
 */
int register_host_mmio_handler(unsigned long start, unsigned long end,
	mmio_handler_t read, mmio_handler_t write)
{
	struct pkvm_mmio_emul_table *table;
	struct pkvm_mmio_handler *handler;
	unsigned long index;
	int ret = 0;

	if (start > end)
		return -EINVAL;

	table = &host_mmio_emul_table;

	if (emul_mmio_lookup(table, start, end))
		return -EINVAL;

	index = find_first_zero_bit(table->bitmap, PKVM_MAX_MMIO_EMUL_NUM);
	if (index >= PKVM_MAX_MMIO_EMUL_NUM)
		return -ENOSPC;

	__set_bit(index, table->bitmap);

	handler = &table->table[index];
	handler->start = start;
	handler->end = end;
	handler->read = read;
	handler->write = write;

	host_ept_lock();
	ret = pkvm_host_ept_unmap(start, start, end - start + 1);
	host_ept_unlock();

	return ret;
}

/*
 * mmcfg access in x86 only use simple mov instrcutions. So keep the decoder
 * simple for now.
 * TODO: make the decoder complete
 */
static int mmio_instruction_decode(struct kvm_vcpu *vcpu, unsigned long gpa,
	struct pkvm_mmio_req *req)
{
	struct x86_exception exception;
	bool direction, zero_extend = false;
	unsigned long rip;
	u8 insn[3];
	int size;

	rip = vmcs_readl(GUEST_RIP);

	/*
	 * Read first three bytes is enough to determine the opcode.
	 * Check arch/x86/include/asm/pci_x86.h.
	 */
	if (read_gva(vcpu, rip, insn, 3, &exception) < 0)
		return -EINVAL;

	/*
	 * In case the compiler adds the REX prefix
	 */
	if ((insn[0] & 0xf0) == 0x40) {
		insn[0] = insn[1];
		insn[1] = insn[2];
	}

	if (insn[0] == 0x66 && (insn[1] & 0xf0) == 0x40)
		insn[1] = insn[2];

	switch (insn[0]) {
	case 0x0f:
		switch (insn[1]) {
		case 0xb6:
			zero_extend = true;
			direction = PKVM_IO_READ;
			size = 1;
			break;
		default:
			return -EIO;
		}
		break;
	case 0x66:
		size = 2;
		switch (insn[1]) {
		case 0x89:
			direction = PKVM_IO_WRITE;
			break;
		case 0x8b:
			direction = PKVM_IO_READ;
			break;
		default:
			return -EIO;
		}
		break;
	case 0x88:
		size = 1;
		direction = PKVM_IO_WRITE;
		break;
	case 0x89:
		size = 4;
		direction = PKVM_IO_WRITE;
		break;
	case 0x8a:
		size = 1;
		direction = PKVM_IO_READ;
		break;
	case 0x8b:
		size = 4;
		direction = PKVM_IO_READ;
		break;
	default:
		return -EIO;
	}

	req->address = gpa;
	req->size = size;
	req->value = &vcpu->arch.regs[VCPU_REGS_RAX];
	req->direction = direction;

	if (zero_extend)
		*req->value = 0;

	return 0;
}

static struct pkvm_mmio_handler *get_mmio_handler(struct pkvm_mmio_emul_table *table,
	struct pkvm_mmio_req *req)
{
	struct pkvm_mmio_handler *handler;
	unsigned long start, end;

	start = req->address;
	end = req->address + req->size - 1;

	handler = emul_mmio_lookup(table, start, end);

	/*
	 * If handler is NULL, this is an access that does not touch the emulated
	 * MMIO range. Return the default handler.
	 */
	if (!handler)
		return &default_mmio_handler;

	/* Do not allow the access to cross the boundary. */
	if ((start < handler->start && end >= handler->start) ||
		(start <= handler->end && end > handler->end))
		return NULL;

	return handler;
}

static int emulate_host_mmio(struct kvm_vcpu *vcpu, struct pkvm_mmio_req *req)
{
	struct pkvm_mmio_emul_table *table;
	struct pkvm_mmio_handler *handler;
	int ret = 0;

	table = &host_mmio_emul_table;

	handler = get_mmio_handler(table, req);
	if (!handler)
		return -EINVAL;

	if (req->direction == PKVM_IO_READ && handler->read)
		ret = handler->read(vcpu, req);
	else if (req->direction == PKVM_IO_WRITE && handler->write)
		ret = handler->write(vcpu, req);

	return ret;
}

static int handle_host_mmio(struct kvm_vcpu *vcpu, unsigned long gpa)
{
	struct pkvm_mmio_req req;

	if (mmio_instruction_decode(vcpu, gpa, &req)) {
		pkvm_dbg("pkvm: MMIO instruction decode failed");
		return -EINVAL;
	}

	pkvm_dbg("pkvm: host %s MMIO gpa 0x%lx width %d value 0x%lx", req.direction ?
		"write" : "read", req.address, req.size, *req.value);

	return emulate_host_mmio(vcpu, &req);
}

int try_emul_host_mmio(struct kvm_vcpu *vcpu, unsigned long gpa)
{
	if (emul_mmio_lookup(&host_mmio_emul_table, gpa, gpa) == NULL)
		return -EINVAL;

	if (handle_host_mmio(vcpu, gpa)) {
		pkvm_err("%s: emulate MMIO failed for memory address 0x%lx\n", __func__, gpa);
		return -EIO;
	}

	return 0;
}
