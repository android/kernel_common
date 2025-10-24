// SPDX-License-Identifier: GPL-2.0

#include <linux/export.h>
#include <linux/refcount.h>

__rust_helper refcount_t rust_helper_REFCOUNT_INIT(int n)
{
	return (refcount_t)REFCOUNT_INIT(n);
}

__rust_helper void rust_helper_refcount_inc(refcount_t *r)
{
	refcount_inc(r);
}

__rust_helper bool rust_helper_refcount_dec_and_test(refcount_t *r)
{
	return refcount_dec_and_test(r);
}
