/* SPDX-License-Identifier: GPL-2.0 */

#include <asm/pkvm_image.h>
#include <asm-generic/vmlinux.lds.h>
#include <asm/cache.h>
#include <asm/page.h>

SECTIONS {
	/* FIXME:
	 * According to arch/x86/kernel/vmlinux.lds.S, the section
	 * ".text..__x86.rethunk_untrain" starts at 2M aligned address,
	 * followed by ".text..__x86.rethunk_safe" section. Since those two
	 * sections for pkvm are merged in .pkvm.text below, need to check
	 * whether the same alignment should be enforced.
	 */
	PKVM_SECTION(.text)
	PKVM_SECTION(.noinstr.text)
	PKVM_SECTION(.init.text)
	PKVM_SECTION(.rodata)
	/*
	 * .pkvm.data..percpu needs to be defined before .data section
	 * as .pkvm.data section will include all sections started with
	 * .data which will include .data..percpu as well.
	 *
	 * .pkvm.data..percpu also needs to be page aligned to maintain
	 * the same alignment for when linking into vmlinux.
	 */
	. = ALIGN(PAGE_SIZE);
	BEGIN_PKVM_SECTION(.data..percpu)
		. = ALIGN(INTERNODE_CACHE_BYTES);
		*(.data..percpu..read_mostly)
		. = ALIGN(INTERNODE_CACHE_BYTES);
		*(.data..percpu)
		*(.data..percpu..shared_aligned)
	END_PKVM_SECTION
	PKVM_SECTION(.data)
	PKVM_SECTION(.init.data)
	PKVM_SECTION(.bss)
}
