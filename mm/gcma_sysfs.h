/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GCMA_SYSFS_H__
#define __GCMA_SYSFS_H__

#ifdef CONFIG_GCMA_SYSFS
void gcma_stat_inc(enum gcma_stat_type type);
void gcma_stat_dec(enum gcma_stat_type type);
void gcma_stat_add(enum gcma_stat_type type, unsigned long delta);
void gcma_stat_sub(enum gcma_stat_type type, unsigned long delta);
#else /* CONFIG_GCMA_SYSFS */
static inline void gcma_stat_inc(enum gcma_stat_type type) {}
static inline void gcma_stat_dec(enum gcma_stat_type type) {}
static inline void gcma_stat_add(enum gcma_stat_type type,
				 unsigned long delta) {}
static inline void gcma_stat_sub(enum gcma_stat_type type,
				 unsigned long delta) {}
#endif /* CONFIG_GCMA_SYSFS */

#endif
