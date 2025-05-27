// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/init.h>

static int __init rbd_init(void)
{
	pr_warn("Using Rust Binder dummy module");
	return 0;
}

module_init(rbd_init);
MODULE_DESCRIPTION("Dummy Rust Binder module");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alice Ryhl <aliceryhl@google.com>");
