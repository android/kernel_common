// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <nvhe/clock.h>

#include <asm/arch_timer.h>
#include <asm/div64.h>

static struct clock_data {
	struct {
		u32 mult;
		u32 shift;
		u64 epoch_ns;
		u64 epoch_cyc;
	} data[2];
	u64 cur;
} trace_clock_data;

/* Does not guarantee no reader on the modified bank. */
void trace_clock_update(u32 mult, u32 shift, u64 epoch_ns, u64 epoch_cyc)
{
	struct clock_data *clock = &trace_clock_data;
	u64 bank = clock->cur ^ 1;

	clock->data[bank].mult		= mult;
	clock->data[bank].shift		= shift;
	clock->data[bank].epoch_ns	= epoch_ns;
	clock->data[bank].epoch_cyc	= epoch_cyc;

	smp_store_release(&clock->cur, bank);
}

/* Using host provided data. Do not use for anything else than debugging. */
u64 __attribute__((patchable_function_entry(0, 0))) trace_clock(void)
{
	struct clock_data *clock = &trace_clock_data;
	u64 bank = smp_load_acquire(&clock->cur);
	u64 cyc, ns;

	cyc = __arch_counter_get_cntpct() - clock->data[bank].epoch_cyc;

	ns = cyc * clock->data[bank].mult;
	ns >>= clock->data[bank].shift;

	return (u64)ns + clock->data[bank].epoch_ns;
}
