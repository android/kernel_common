// SPDX-License-Identifier: GPL-2.0

/*
 *  This module is used to:
 *
 * - Print the number of pages available in the system.
 * - Print the number of PFNs in the system.
 * - Print the address of vmemmap which it is the same that pfn_to_page(0).
 */

#define pr_fmt(fmt) "%s: %s: " fmt, KBUILD_MODNAME, __func__

#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/types.h>

/*
 * Print the start and end of the RAM as well as the size.
 *
 * This information is found in the device tree.
 *
 *   memory@40000000 {
 *       reg = <0x00 0x40000000 0x02 0x00>;
 *       device_type = "memory";
 *   };
 */
static void print_ram_info(void)
{
	phys_addr_t start_addr = memblock_start_of_DRAM();
	phys_addr_t end_addr = memblock_end_of_DRAM();
	phys_addr_t mem_size = memblock_phys_mem_size();
	// What does the reserved memory includes.
	phys_addr_t reserved_size = memblock_reserved_size();

	pr_info("    start_addr:    %10llu == %#010Lx", (u64)start_addr, (u64)start_addr);
	pr_info("    end_addr:      %10llu == %#010Lx", (u64)end_addr, (u64)end_addr);
	pr_info("    mem_size:      %10llu == %#010Lx", (u64)mem_size, (u64)mem_size);
	pr_info("    reserved_size: %10llu == %#010Lx", (u64)reserved_size, (u64)reserved_size);
}

static int __init mem_phys_layout_init(void)
{
	print_ram_info();

	return 0;
}

static void __exit mem_phys_layout_exit(void)
{
	pr_info("Leaving mem_phys_layout module\n");
}

module_init(mem_phys_layout_init);
module_exit(mem_phys_layout_exit);

MODULE_AUTHOR("Juan Yescas");
MODULE_DESCRIPTION("Nodes, Zones and Physical Memory Model");
MODULE_LICENSE("GPL v2");



