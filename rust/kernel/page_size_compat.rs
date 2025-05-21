// SPDX-License-Identifier: GPL-2.0

//! x86_64 page size compat
//!
//! C header: [`include/linux/page_size_compat.h`](srctree/include/linux/page_size_compat.h).
//!
//! Rust helpers for x86_64 page size emulation.

use kernel::{bindings, jump_label, page};

/// __page_shift is the emulated page shift.
#[inline(always)]
pub fn __page_shift() -> usize {
    // SAFETY: Accessing C static key
    let emulated = unsafe {
        jump_label::static_branch_unlikely!(
            bindings::page_shift_compat_enabled,
            bindings::static_key_false,
            key
        )
    };

    if emulated {
        // SAFETY: Reading static C variable page_shift_compat.
        // page_size_compat is marked __ro_after_init in the C code.
        let shift = unsafe { bindings::page_shift_compat };

        // page_shift_compat is sanitized and range checked by boot
        // parameter parsing; so this conversion should always be safe.
        shift.try_into().unwrap()
    } else {
        page::PAGE_SHIFT
    }
}

/// __page_size is the emulated page size.
#[inline(always)]
pub fn __page_size() -> usize {
    let shift = __page_shift();

    1usize << shift
}

/// __page_mask can be used to align addresses to a __page boundary.
#[inline(always)]
pub fn __page_mask() -> usize {
    let page_size = __page_size();

    !(page_size - 1)
}

/// Aligns the given address UP to the nearest __page boundary.
#[inline(always)]
pub fn __page_align(addr: usize) -> usize {
    let page_size = __page_size();
    let mask = !(page_size - 1);

    // Parentheses around `page_size - 1` to avoid triggering
    // overflow sanitizers in the wrong cases.
    (addr + (page_size - 1)) & mask
}

/// Aligns the given address DOWN to the nearest __page boundary.
#[inline(always)]
pub fn __page_align_down(addr: usize) -> usize {
    let mask = __page_mask();

    addr & mask
}
