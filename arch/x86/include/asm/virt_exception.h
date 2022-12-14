/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_VIRT_EXCEPTION_H
#define _ASM_X86_VIRT_EXCEPTION_H

#include <asm/ptrace.h>

#ifndef __ASSEMBLY__

/*
 * Used by the #VE exception handler to gather the #VE exception
 * info from the TDX module. This is a software only structure
 * and not part of the TDX module/VMM ABI.
 */
struct ve_info {
	u64 exit_reason;
	u64 exit_qual;
	/* Guest Linear (virtual) Address */
	u64 gla;
	/* Guest Physical Address */
	u64 gpa;
	u32 instr_len;
	u32 instr_info;
};

int ve_handle_mmio(struct pt_regs *regs, struct ve_info *ve);

bool mmio_read(int size, unsigned long addr, unsigned long *val);
bool mmio_write(int size, unsigned long addr, unsigned long val);

#endif /* !__ASSEMBLY__ */
#endif /* _ASM_X86_VIRT_EXCEPTION_H */
