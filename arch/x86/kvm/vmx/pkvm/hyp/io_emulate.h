/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2023 Intel Corporation
 */
#ifndef _PKVM_IO_EMULATE_H_
#define _PKVM_IO_EMULATE_H_

/* I/O direction */
#define PKVM_IO_READ 0
#define PKVM_IO_WRITE 1

/* Max num of port I/O emulation handlers */
#define PKVM_MAX_PIO_EMUL_NUM 32

struct pkvm_pio_req {
	unsigned int port;
	int size;
	bool direction;
	unsigned long *value;
};

typedef int (*pio_handler_t)(struct kvm_vcpu *, struct pkvm_pio_req *);

struct pkvm_pio_handler {
	unsigned int port;
	int size_mask;
	pio_handler_t read;
	pio_handler_t write;
};

struct pkvm_pio_emul_table {
	struct pkvm_pio_handler table[PKVM_MAX_PIO_EMUL_NUM];
	DECLARE_BITMAP(bitmap, PKVM_MAX_PIO_EMUL_NUM);
};

int register_host_pio_handler(struct pkvm_host_vm *host_vm, unsigned int port,
	unsigned int size_mask, pio_handler_t read, pio_handler_t write);

#endif
