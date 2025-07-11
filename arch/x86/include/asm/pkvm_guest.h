/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PKVM_GUEST_H
#define _ASM_X86_PKVM_GUEST_H

#include <linux/types.h>

#ifdef CONFIG_PKVM_GUEST

#include <asm/shared/tdx.h>

u64 __pkvm_module_call(u64 fn, struct tdx_module_args *out);

void pkvm_guest_init_coco(void);
int pkvm_set_mem_host_visibility(unsigned long addr, int numpages, bool enc);
bool pkvm_is_protected_guest(void);

#else

static inline void pkvm_guest_init_coco(void) {}
static inline int pkvm_set_mem_host_visibility(unsigned long addr,
					       int numpages, bool enc)
{
	return 0;
}

static inline bool pkvm_is_protected_guest(void) { return false; }

#endif

#endif
