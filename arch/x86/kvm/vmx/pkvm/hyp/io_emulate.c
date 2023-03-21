// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2023 Intel Corporation. */
#include <pkvm.h>
#include "io_emulate.h"

struct pkvm_pio_emul_table host_pio_emul_table;

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
