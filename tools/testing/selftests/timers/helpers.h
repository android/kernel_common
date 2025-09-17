/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SELFTESTS_TIMERS_HELPERS_H
#define SELFTESTS_TIMERS_HELPERS_H

#include <time.h>

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000L
#endif

/*
 * timespec_to_ns - Convert timespec to nanoseconds
 */
static inline long long timespec_to_ns(const struct timespec *ts)
{
	return ((long long) ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}

/*
 * timespec_sub - Subtract two timespec values
 *
 * @a: first timespec
 * @b: second timespec
 *
 * Returns a - b in nanoseconds.
 */
static inline long long timespec_sub(struct timespec a, struct timespec b)
{
	return timespec_to_ns(&a) - timespec_to_ns(&b);
}

#endif /* SELFTESTS_TIMERS_HELPERS_H */
