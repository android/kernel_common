/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __BIONIC_COMPAT_H
#define __BIONIC_COMPAT_H

#define _GNU_SOURCE
#include <sys/types.h>

static inline int pthread_cancel(pthread_t thread)
{
	return 0;
}

#endif /* __BIONIC_COMPAT_H */
