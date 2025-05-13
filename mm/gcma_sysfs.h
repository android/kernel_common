/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GCMA_SYSFS_H__
#define __GCMA_SYSFS_H__

enum gcma_stat_type {
        STORED_PAGE,
        LOADED_PAGE,
        EVICTED_PAGE,
        CACHED_PAGE,
        DISCARDED_PAGE,
        NUM_OF_GCMA_STAT,
};

#ifdef CONFIG_GCMA_SYSFS
void gcma_stat_inc(enum gcma_stat_type type);
void gcma_stat_dec(enum gcma_stat_type type);
void gcma_stat_add(enum gcma_stat_type type, unsigned long delta);
#else /* CONFIG_GCMA_SYSFS */
static inline void gcma_stat_inc(enum gcma_stat_type type) {}
static inline void gcma_stat_dec(enum gcma_stat_type type) {}
static inline void gcma_stat_add(enum gcma_stat_type type,
				 unsigned long delta) {}
#endif /* CONFIG_GCMA_SYSFS */

#endif
