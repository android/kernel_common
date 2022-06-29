// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (C) 2018-2022 Intel Corporation
 *
 * pkvm runs in a self-contained environment
 * and requires a self-contained spinlock implementation
 * which doesn't rely on any other external symbols.
 *
 * This is arch specific implementation
 */
#ifndef _X86_ASM_PKVM_SPINLOCK_H
#define _X86_ASM_PKVM_SPINLOCK_H

#include <linux/types.h>

typedef union pkvm_spinlock {
	u64	__val;
	struct {
		u32 head;
		u32 tail;
	};
} pkvm_spinlock_t;

#define __PKVM_SPINLOCK_UNLOCKED 			\
	((pkvm_spinlock_t){ .__val = 0 })

#define pkvm_spin_lock_init(l) 				\
do {							\
	*(l) = __PKVM_SPINLOCK_UNLOCKED;		\
} while (0);

static inline void pkvm_spin_lock(pkvm_spinlock_t *lock)
{
	/* The lock function atomically increments and exchanges the head
	 * counter of the queue. If the old head of the queue is equal to the
	 * tail, we have locked the spinlock. Otherwise we have to wait.
	 */

	asm volatile ("   movl $0x1,%%eax\n"
		      "   lock xaddl %%eax,%[head]\n"
		      "   cmpl %%eax,%[tail]\n"
		      "   jz 1f\n"
		      "2: pause\n"
		      "   cmpl %%eax,%[tail]\n"
		      "   jnz 2b\n"
		      "1:\n"
		      :
		      :
		      [head] "m"(lock->head),
		      [tail] "m"(lock->tail)
		      : "cc", "memory", "eax");
}

static inline void pkvm_spin_unlock(pkvm_spinlock_t *lock)
{
	/* Increment tail of queue */
	asm volatile ("   lock incl %[tail]\n"
				:
				: [tail] "m" (lock->tail)
				: "cc", "memory");

}

/*
 * Redefine for virt/kvm/pkvm/page_alloc.c usage
 * TODO: unify the API name: pkvm_ vs. hyp_ ?
 */
#define hyp_spinlock_t pkvm_spinlock_t
#define hyp_spin_lock_init pkvm_spin_lock_init
#define hyp_spin_lock pkvm_spin_lock
#define hyp_spin_unlock pkvm_spin_unlock

#endif
