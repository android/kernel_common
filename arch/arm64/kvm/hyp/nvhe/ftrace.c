// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2025 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <nvhe/mm.h>

#include <asm/kvm_mmu.h>
#include <asm/patching.h>

#include <asm/kvm_hypevents.h>

#define HYP_FTRACE_MAX_OFFSETS	17 /* MAX_MOD_EVENTS + 1 */
#define HYP_FTRACE_MAX_DEPTH	32

extern unsigned long hyp_nr_cpus;

extern void __hyp_ftrace_tramp(void);
extern void __hyp_ftrace_ret_tramp(void);

static unsigned long hyp_kern_offsets[HYP_FTRACE_MAX_OFFSETS];

static unsigned long __kern_addr(unsigned long offset_idx, unsigned long addr)
{
	return addr + hyp_kern_offsets[offset_idx];
}

struct hyp_ftrace_stack_frame {
	unsigned long	func;
	unsigned long	ret;
};

struct hyp_ftrace_stack {
	int				idx;
	struct hyp_ftrace_stack_frame	frames[HYP_FTRACE_MAX_DEPTH];
};

static DEFINE_PER_CPU(struct hyp_ftrace_stack, __ftrace_saved_frames);

static void hyp_ftrace_func_reset(void)
{
	unsigned int cpu;

	for (cpu = 0; cpu < hyp_nr_cpus; cpu++) {
		struct hyp_ftrace_stack *stack;

		stack = per_cpu_ptr(&__ftrace_saved_frames, cpu);
		stack->idx = -1;
	}

	/*
	 * Make sure the stack init is observed by all CPUs before patching the
	 * code. Paired with smp_load_acquire() in hyp_ftrace_func_push().
	 */
	smp_mb();
}

static __always_inline bool hyp_ftrace_func_push(unsigned long func, unsigned long ret)
{
	struct hyp_ftrace_stack *stack = this_cpu_ptr(&__ftrace_saved_frames);
	int idx = smp_load_acquire(&stack->idx);

	if (idx >= (HYP_FTRACE_MAX_DEPTH - 1))
		return false;

	idx++;
	stack->frames[idx].func = func;
	stack->frames[idx].ret = ret;
	stack->idx = idx;

	return true;
}

static __always_inline struct hyp_ftrace_stack_frame *hyp_ftrace_func_pop(void)
{
	struct hyp_ftrace_stack *stack = this_cpu_ptr(&__ftrace_saved_frames);

	/*
	 * If in _pop(), then _push() has run on this CPU. No need for more
	 * memory ordering.
	 */

	if (stack->idx < 0)
		return NULL;

	return &stack->frames[stack->idx--];
}

unsigned long __hyp_ftrace_trace(unsigned long ip, unsigned long parent,
				 unsigned long offset_idx)
{
	unsigned long func = __kern_addr(offset_idx, ip);
	unsigned long parent_offset_idx;

	/* When modules are called from core */
	parent_offset_idx = parent > (unsigned long)__hyp_text_start ? 0 : offset_idx;

	trace_func(func, __kern_addr(parent_offset_idx, parent));

	/* Only install the trampoline if we can revert to the original parent */
	if (hyp_ftrace_func_push(func, parent))
		return (unsigned long)__hyp_ftrace_ret_tramp;

	return parent;
}

unsigned long __hyp_ftrace_ret_trace(void)
{
	struct hyp_ftrace_stack_frame *frame = hyp_ftrace_func_pop();

	BUG_ON(!frame);
	trace_func_ret(frame->func);

	return frame->ret;
}

void hyp_ftrace_ret_flush(void)
{
	struct hyp_ftrace_stack_frame *frame = hyp_ftrace_func_pop();

	while (frame) {
		trace_func_ret(frame->func);
		frame = hyp_ftrace_func_pop();
	}
}

static int __get_offset_idx_ins(unsigned long *func, unsigned long ip, u32 *insn,
				void *args)
{
	unsigned long idx = (unsigned long)args;
	u32 imm, mask = (BIT(16) - 1) << 5;

	imm = (idx << 5) & mask;

	*insn = aarch64_insn_get_movz_value();
	*insn |= BIT(31);			/* 64-bits variant */
	*insn |= 10;				/* x10 */
	*insn &= ~mask;
	*insn |= imm;
	*insn = cpu_to_le32(*insn);

	return 0;
}

static int __get_disable_ins(unsigned long *func, unsigned long ip, u32 *insn,
			     void *args)
{
	static u32 nop;

	if (!nop)
		nop = aarch64_insn_gen_nop();

	*insn = cpu_to_le32(nop);

	return 0;
}

static int __get_enable_ins(unsigned long ip, u32 *insn, void *tramp)
{
	u32 imm, mask;
	long delta;

	delta = (long)tramp - (long)ip;

	if (delta > SZ_128M || delta <= -SZ_128M)
		return -ERANGE;

	mask = BIT(26) - 1;
	imm = (delta >> 2) & mask;

	*insn = aarch64_insn_get_bl_value() & ~(mask);
	*insn |= imm;
	*insn = cpu_to_le32(*insn);

	return 0;
}

#define funcs_pg_enabled(func)	((func) & 0x1)
#define funcs_pg_func(func)	((func) & ~BIT(0))
#define funcs_pg_is_end(func)						      \
({									      \
	(!(*(func)) ||							      \
	 ((PAGE_ALIGN((unsigned long)(func) + 1) - (unsigned long)(func)) <= 8)); \
})

/*
 * During init the kernel can notify a function needs to be enabled. This is
 * relying on the same encoding as the func_pg.
 */
#define get_func(func)		funcs_pg_func(func)

static int __get_enable_disable_ins_early(unsigned long *func, unsigned long ip,
					  u32 *insn, void *tramp)
{
	if (funcs_pg_enabled(*func))
		return __get_enable_ins(ip, insn, tramp);

	/* Nothing else to do */
	return 1;
}

struct __ftrace_sync_patch_args {
	void		*tramp;
	unsigned long	offset_idx;
	unsigned long	*funcs_pg;
};

static int
__get_enable_disable_ins_from_funcs_pg(unsigned long *func, unsigned long ip,
				       u32 *insn, void *__args)
{
	struct __ftrace_sync_patch_args *args = __args;
	unsigned long kern_addr;
	static u32 nop;
	u32 cur_insn;
	bool enable;
	int ret = 0;

	if (funcs_pg_is_end(args->funcs_pg))
		return -EAGAIN;

	kern_addr = __kern_addr(args->offset_idx, *func);
	if (get_func(kern_addr) != funcs_pg_func(*args->funcs_pg)) {
		ret = -EINVAL;
		goto end;
	}

	if (!nop)
		nop = aarch64_insn_gen_nop();

	enable = funcs_pg_enabled(*args->funcs_pg);
	cur_insn = *(u32 *)ip;

	/* Are we modifying anything? */
	if ((cur_insn == nop) != enable) {
		ret = -EBUSY;
		goto end;
	}

	if (funcs_pg_enabled(*args->funcs_pg))
		ret = __get_enable_ins(ip, insn, args->tramp);
	else
		*insn = cpu_to_le32(nop);

end:
	args->funcs_pg++;
	return ret;
}

phys_addr_t __get_phys(unsigned long addr)
{
	if (addr >= (unsigned long)__hyp_text_start)
		return __hyp_pa(addr);

	return __pkvm_private_range_pa((void *)addr);
}

#define HYP_FTRACE_SKIP_FUNC (-1ULL)

static void hyp_ftrace_patch(unsigned long *funcs, unsigned long *funcs_end,
			     size_t func_offset,
			     int (*get_ins)(unsigned long *func, unsigned long ip,
					    u32 *insn, void *args),
			     void *args)
{
	unsigned long prev_ip;
	void *map = NULL;

	while (funcs < funcs_end) {
		unsigned long ip;
		size_t delta;
		u32 insn;

		if (!*funcs)
			break;

		if (*funcs == HYP_FTRACE_SKIP_FUNC)
			goto next;

		ip = get_func(*funcs) + func_offset;
		delta = ip - prev_ip;

		if (!map) {
			map = hyp_fixmap_map(__get_phys(ip));
		} else if ((unsigned long)(map + delta) >=
			   PAGE_ALIGN((unsigned long)map + 4)) {
			hyp_fixmap_unmap();
			map = hyp_fixmap_map(__get_phys(ip));
		} else {
			map = (void *)PAGE_ALIGN_DOWN((unsigned long)map) +
					      offset_in_page(ip);
		}

		prev_ip = ip;

		if (get_ins(funcs, ip, &insn, args))
			goto next;

		WRITE_ONCE(*(u32 *)map, insn);

		caches_clean_inval_pou((unsigned long)map,
				       (unsigned long)map + AARCH64_INSN_SIZE);
next:
		funcs++;
	}

	if (map)
		hyp_fixmap_unmap();
}

int hyp_ftrace_setup(unsigned long *funcs, unsigned long *funcs_end,
		     unsigned long hyp_kern_offset, void *tramp)
{
	unsigned long idx;

	for (idx = 0; idx < HYP_FTRACE_MAX_OFFSETS; idx++) {
		if (!hyp_kern_offsets[idx])
			break;
	}

	if (idx >= HYP_FTRACE_MAX_OFFSETS)
		return -ENOMEM;

	hyp_kern_offsets[idx] = hyp_kern_offset;

	hyp_ftrace_patch(funcs, funcs_end, AARCH64_INSN_SIZE,
			 __get_offset_idx_ins, (void *)idx);

	hyp_ftrace_patch(funcs, funcs_end, 2 * AARCH64_INSN_SIZE,
			 __get_enable_disable_ins_early, tramp);

	return idx;
}

extern unsigned long __hyp_patchable_function_entries_start[];
extern unsigned long __hyp_patchable_function_entries_end[];

unsigned long __hyp_text_start_kern;

void hyp_ftrace_setup_core(void)
{
	hyp_ftrace_func_reset();

	hyp_ftrace_setup(__hyp_patchable_function_entries_start,
			 __hyp_patchable_function_entries_end,
			 __hyp_text_start_kern - (unsigned long)__hyp_text_start,
			 __hyp_ftrace_tramp);
}

unsigned long *hyp_ftrace_find_host_func(unsigned long host_func,
					 unsigned long *funcs,
					 unsigned long *funcs_end,
					 unsigned long offset_idx)
{
	if (!funcs) {
		funcs = __hyp_patchable_function_entries_start;
		funcs_end = __hyp_patchable_function_entries_end;
		offset_idx = 0;
	}

	while (funcs < funcs_end) {
		unsigned long kern_addr = __kern_addr(offset_idx, *funcs);

		if (get_func(kern_addr) == funcs_pg_func(host_func))
			return funcs;

		funcs++;
	}

	return NULL;
}

/*
 * funcs_pg is the host donated page containing the list of functions to
 * enable/disable.
 *
 * funcs and funcs_end are the hypervisor owned ELF sections. For security
 * purposes, funcs_pg is validated against funcs/funcs_end and for efficency
 * purposes, it is expected from funcs_pg to have the same order as
 * funcs/funcs_end.
 *
 * Returns NULL if the entire funcs_pg has been consumed otherwise the next
 * entry to process if funcs_end has been reached.
 */
void *hyp_ftrace_sync(unsigned long *funcs_pg, unsigned long *funcs,
		      unsigned long *funcs_end, unsigned long offset_idx,
		      void *tramp)
{
	struct __ftrace_sync_patch_args args = {
		.tramp = tramp ? tramp : (void *)__hyp_ftrace_tramp,
		.offset_idx = funcs ? offset_idx : 0,
		.funcs_pg = funcs_pg,
	};

	if (!funcs_end)
		funcs_end = __hyp_patchable_function_entries_end;

	hyp_ftrace_patch(funcs, funcs_end, 2 * AARCH64_INSN_SIZE,
			 __get_enable_disable_ins_from_funcs_pg, (void *)&args);

	return funcs_pg_is_end(args.funcs_pg) ? NULL : args.funcs_pg;
}

void hyp_ftrace_disable(unsigned long *funcs, unsigned long *funcs_end)
{
	if (!funcs || !funcs_end) {
		funcs = __hyp_patchable_function_entries_start;
		funcs_end = __hyp_patchable_function_entries_end;
	}

	hyp_ftrace_patch(funcs, funcs_end, 2 * AARCH64_INSN_SIZE,
			 __get_disable_ins, NULL);
}
