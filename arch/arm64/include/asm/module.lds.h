/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/page-def.h>

SECTIONS {
	.plt 0 : { BYTE(0) }
	.init.plt 0 : { BYTE(0) }
	.text.ftrace_trampoline 0 : { BYTE(0) }

#ifdef CONFIG_KASAN_SW_TAGS
	/*
	 * Outlined checks go into comdat-deduplicated sections named .text.hot.
	 * Because they are in comdats they are not combined by the linker and
	 * we otherwise end up with multiple sections with the same .text.hot
	 * name in the .ko file. The kernel module loader warns if it sees
	 * multiple sections with the same name so we use this sections
	 * directive to force them into a single section and silence the
	 * warning.
	 */
	.text.hot : { *(.text.hot) }
#endif

#ifdef CONFIG_UNWIND_TABLES
	/*
	 * Currently, we only use unwind info at module load time, so we can
	 * put it into the .init allocation.
	 */
	.init.eh_frame : { *(.eh_frame) }
#endif

#ifdef CONFIG_KVM
	.hyp.text : ALIGN(PAGE_SIZE) {
		*(.hyp.text)
		*(.hyp.text.ftrace_tramp)
		. = ALIGN(PAGE_SIZE);
	}
	.hyp.bss : ALIGN(PAGE_SIZE) {
		*(.hyp.bss)
		. = ALIGN(PAGE_SIZE);
	}
	.hyp.rodata : ALIGN(PAGE_SIZE) {
		*(.hyp.rodata)
		. = ALIGN(PAGE_SIZE);
	}
	.hyp.event_ids : ALIGN(PAGE_SIZE) {
		/*
		 * Yet empty, without that *(.hyp.event_ids) input section
		 * (named after the output section), the location counter
		 * page-alignment below is ignored.
		 */
		*(.hyp.event_ids)
		*(SORT(.hyp.event_ids.*))
		*(.hyp.printk_fmt_offset)
		. = ALIGN(PAGE_SIZE);
	}
	.hyp.patchable_function_entries : ALIGN(PAGE_SIZE) {
		*(.hyp.patchable_function_entries)
		. = ALIGN(PAGE_SIZE);
	}
	.hyp.data : ALIGN(PAGE_SIZE) {
		*(.hyp.data)
		. = ALIGN(PAGE_SIZE);
	}
	.hyp.reloc : ALIGN(4) {	*(.hyp.reloc) }
	_hyp_events : { *(SORT(_hyp_events.*)) }
#endif
}
