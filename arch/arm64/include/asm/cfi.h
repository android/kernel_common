/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_CFI_H
#define _ASM_ARM64_CFI_H

#ifdef CONFIG_CFI_CLANG
#define __bpfcall
static inline int cfi_get_offset(void)
{
	return 4;
}
#define cfi_get_offset cfi_get_offset
extern u32 cfi_bpf_hash;
extern u32 cfi_bpf_subprog_hash;
extern u32 cfi_get_func_hash(void *func);
#else
#define cfi_bpf_hash 0U
#define cfi_bpf_subprog_hash 0U
static inline u32 cfi_get_func_hash(void *func)
{
	return 0;
}
#endif /* CONFIG_CFI_CLANG */
#endif /* _ASM_ARM64_CFI_H */
