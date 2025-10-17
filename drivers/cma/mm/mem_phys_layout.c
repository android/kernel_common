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
#include <linux/nodemask.h>
#include <linux/pfn.h>
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

// Prints the node information in:
//
//     struct pglist_data *node_data;
//
// It uses NODE_DATA to access node_data.
//
//     #define NODE_DATA(nid)		(node_data[(nid)])
static void print_node_data(void)
{
	// Note: NUMA nodes are configured in the device tree.
	int nodeId;
	unsigned long phys_pages = get_num_physpages();

	for_each_online_node(nodeId) {
		// Number of Physical pages including holes in the node.
		uint64_t num_spanned_pages = NODE_DATA(nodeId)->node_spanned_pages;
		// Number of Physical pages in the node.
		uint64_t num_present_pages = NODE_DATA(nodeId)->node_present_pages;
		uint64_t start_pfn = NODE_DATA(nodeId)->node_start_pfn;
		uint64_t end_pfn = NODE_DATA(nodeId)->node_start_pfn + num_spanned_pages;

		pr_info("Node %d", nodeId);
		pr_info("    Page frames numbers (range): [pfn %#010Lx-%#010Lx)\n",
				start_pfn, end_pfn);
		pr_info("    Number of Page Spanned pages (includes holes): %#010Lx : %llu\n",
				num_spanned_pages, num_spanned_pages);
		pr_info("    Number of Page Present pages:                  %#010Lx : %llu",
				num_present_pages, num_present_pages);
		pr_info("    Number of Page frames (end_pfn - start_pfn):   %#010Lx : %llu",
				end_pfn - start_pfn, end_pfn - start_pfn);
	}

	pr_info("Total number of Physical Pages in all nodes (DO NOT include holes) %lu",
			phys_pages);
}

static void print_pfn_start_address(void)
{
	struct page *first_page = pfn_to_page(0);

	pr_info("First struct page starts at: pfn_to_page(0) [mem %#010Lx]\n",
			(unsigned long long)first_page);

#ifdef CONFIG_SPARSEMEM_VMEMMAP
	pr_info("First struct page starts at: vmemmap        [mem %#010Lx]\n",
			(unsigned long long)vmemmap);
#endif
}

static void print_pageblock_info(void)
{
	pr_info("pageblock_order %d", pageblock_order);
	pr_info("pageblock_nr_pages %lu", pageblock_nr_pages);
}

static int __init mem_phys_layout_init(void)
{
	print_ram_info();
	print_node_data();
	print_pfn_start_address();
	print_pageblock_info();

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

