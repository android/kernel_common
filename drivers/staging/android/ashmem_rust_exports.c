// SPDX-License-Identifier: GPL-2.0

/*
 * Symbols exported from the Ashmem Rust driver for loadable kernel modules to use.
 *
 * Copyright (c) 2025, Google LLC.
 */

#include <linux/export.h>

#include "ashmem.h"

/*
 * List symbols that need to be exported to loadable kernel modules below. This is needed because
 * the logic that exports symbols from Rust crates only considers the crates under the rust/
 * directory at the root of the kernel repo. It currently does not support exporting symbols from
 * other crates.
 */
EXPORT_SYMBOL_GPL(is_ashmem_file);
EXPORT_SYMBOL_GPL(ashmem_area_name);
EXPORT_SYMBOL_GPL(ashmem_area_size);
