// SPDX-License-Identifier: GPL-2.0
/*
 * Non-trivial C macros cannot be used in Rust. Similarly, inlined C functions
 * cannot be called either. This file explicitly creates functions ("helpers")
 * that wrap those so that they can be called from Rust.
 *
 * Sorted alphabetically.
 */

#include <linux/compiler_types.h>

#ifdef __BINDGEN__
// Omit `inline` for bindgen as it ignores inline functions.
#define __rust_helper
#else
// The helper functions are all inline functions.
//
// We use `__always_inline` here to bypass LLVM inlining checks, in case the
// helpers are inlined directly into Rust CGUs.
//
// The LLVM inlining checks are false positives:
// * LLVM doesn't want to inline functions compiled with
//   `-fno-delete-null-pointer-checks` with code compiled without.
//   The C CGUs all have this enabled and Rust CGUs don't. Inlining is okay
//   since this is one of the hardening features that does not change the ABI,
//   and we shouldn't have null pointer dereferences in these helpers.
// * LLVM doesn't want to inline functions with different list of builtins. C
//   side has `-fno-builtin-wcslen`; `wcslen` is not a Rust builtin, so they
//   should be compatible, but LLVM does not perform inlining due to attributes
//   mismatch.
// * clang and Rust doesn't have the exact target string. Clang generates
//   `+cmov,+cx8,+fxsr` but Rust doesn't enable them (in fact, Rust will
//   complain if `-Ctarget-feature=+cmov,+cx8,+fxsr` is used). x86-64 always
//   enable these features, so they are in fact the same target string, but
//   LLVM doesn't understand this and so inlining is inhibited. This can be
//   bypassed with `--ignore-tti-inline-compatible`, but this is a hidden
//   option.
#define __rust_helper __always_inline
#endif

#include "binder.c"
#include "blk.c"
#include "bug.c"
#include "build_assert.c"
#include "build_bug.c"
#include "cred.c"
#include "err.c"
#include "fs.c"
#include "jump_label.c"
#include "kunit.c"
#include "mm.c"
#include "mman.c"
#include "mutex.c"
#include "page.c"
#include "pid_namespace.c"
#include "poll.c"
#include "rbtree.c"
#include "refcount.c"
#include "security.c"
#include "signal.c"
#include "slab.c"
#include "spinlock.c"
#include "task.c"
#include "uaccess.c"
#include "vmalloc.c"
#include "wait.c"
#include "workqueue.c"
