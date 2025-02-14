// SPDX-License-Identifier: GPL-2.0

#include <linux/mman.h>

void rust_helper_lockdep_set_class_rwsem(struct rw_semaphore *lock, struct lock_class_key *key,
					 const char *name)
{
	lockdep_set_class_and_name(lock, key, name);
}
