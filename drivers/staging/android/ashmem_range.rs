// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Keeps track of unpinned ranges in an ashmem file.

use crate::{
    ashmem_shrinker::{
        self, CountObjects, ScanObjects, ShrinkControl, Shrinker, ShrinkerBuilder,
        ShrinkerRegistration,
    },
    shmem::ShmemFile,
    AshmemModule,
};
use core::{
    mem::MaybeUninit,
    pin::Pin,
    sync::atomic::{AtomicUsize, Ordering},
};
use kernel::{
    c_str,
    list::{List, ListArc, ListLinks},
    page::PAGE_SIZE,
    prelude::*,
    sync::{GlobalGuard, GlobalLockedBy, UniqueArc},
};

// Only updated with ASHMEM_MUTEX held, but the shrinker will read it without the mutex.
pub(crate) static LRU_COUNT: AtomicUsize = AtomicUsize::new(0);

pub(crate) struct AshmemLru {
    lru_list: List<Range, 0>,
}

/// Represents ownership of the `ASHMEM_MUTEX` lock.
///
/// Using a wrapper struct around `GlobalGuard` so we can add our own methods to the guard.
pub(crate) struct AshmemGuard(pub(crate) GlobalGuard<ASHMEM_MUTEX>);

// These make `AshmemGuard` inherit the behavior of `GlobalGuard`.
impl core::ops::Deref for AshmemGuard {
    type Target = GlobalGuard<ASHMEM_MUTEX>;
    fn deref(&self) -> &GlobalGuard<ASHMEM_MUTEX> {
        &self.0
    }
}
impl core::ops::DerefMut for AshmemGuard {
    fn deref_mut(&mut self) -> &mut GlobalGuard<ASHMEM_MUTEX> {
        &mut self.0
    }
}

impl AshmemGuard {
    fn shrink_range(&mut self, range: &Range, pgstart: usize, pgend: usize) {
        let old_size = range.size(self);
        {
            let inner = range.inner.as_mut(self);
            inner.pgstart = pgstart;
            inner.pgend = pgend;
        }
        let new_size = range.size(self);

        // Only change the counter if the range is on the lru list.
        if !range.purged(self) {
            let mut lru_count = LRU_COUNT.load(Ordering::Relaxed);
            lru_count -= old_size;
            lru_count += new_size;
            LRU_COUNT.store(lru_count, Ordering::Relaxed);
        }
    }

    fn insert_lru(&mut self, range: ListArc<Range>) {
        // Don't insert the range if it's already purged.
        if !range.purged(self) {
            let mut lru_count = LRU_COUNT.load(Ordering::Relaxed);
            lru_count += range.size(self);
            LRU_COUNT.store(lru_count, Ordering::Relaxed);
            self.lru_list.push_front(range);
        }
    }

    fn remove_lru(&mut self, range: &Range) -> Option<ListArc<Range>> {
        // SAFETY: The only list with ID 0 is this list, so the range can't be in some other list
        // with the same ID.
        let ret = unsafe { self.lru_list.remove(range) };

        // Only decrement lru_count if the range was actually in the list.
        if ret.is_some() {
            let mut lru_count = LRU_COUNT.load(Ordering::Relaxed);
            lru_count -= range.size(self);
            LRU_COUNT.store(lru_count, Ordering::Relaxed);
        }

        ret
    }
}

kernel::sync::global_lock! {
    // SAFETY: We call `init` as the very first thing in the initialization of this module, so
    // there are no calls to `lock` before `init` is called.
    pub(crate) unsafe(uninit) static ASHMEM_MUTEX: Mutex<AshmemLru> = AshmemLru {
        lru_list: List::new(),
    };
}

#[pin_data]
pub(crate) struct Range {
    /// prev/next pointers for `Area::unpinned_list`.
    ///
    /// Note that "unpinned" here refers to the ASHMEM_PIN/UNPIN ioctls, which is unrelated to
    /// Rust's concept of pinning.
    #[pin]
    lru: ListLinks<0>,
    #[pin]
    unpinned: ListLinks<1>,
    file: ShmemFile,
    pub(crate) inner: GlobalLockedBy<RangeInner, ASHMEM_MUTEX>,
}

pub(crate) struct RangeInner {
    pub(crate) pgstart: usize,
    pub(crate) pgend: usize,
    pub(crate) purged: bool,
}

impl Range {
    pub(crate) fn set_purged(&self, guard: &mut AshmemGuard) {
        self.inner.as_mut(guard).purged = true;
    }

    pub(crate) fn purged(&self, guard: &AshmemGuard) -> bool {
        self.inner.as_ref(guard).purged
    }

    pub(crate) fn pgstart(&self, guard: &AshmemGuard) -> usize {
        self.inner.as_ref(guard).pgstart
    }

    pub(crate) fn pgend(&self, guard: &AshmemGuard) -> usize {
        self.inner.as_ref(guard).pgend
    }

    pub(crate) fn size(&self, guard: &AshmemGuard) -> usize {
        let inner = self.inner.as_ref(guard);
        inner.pgend - inner.pgstart + 1
    }

    pub(crate) fn is_before_page(&self, page: usize, guard: &AshmemGuard) -> bool {
        let inner = self.inner.as_ref(guard);
        inner.pgend < page
    }

    pub(crate) fn contains_page(&self, page: usize, guard: &AshmemGuard) -> bool {
        let inner = self.inner.as_ref(guard);
        inner.pgstart <= page && inner.pgend >= page
    }

    pub(crate) fn is_superset_of_range(
        &self,
        pgstart: usize,
        pgend: usize,
        guard: &AshmemGuard,
    ) -> bool {
        let inner = self.inner.as_ref(guard);
        inner.pgstart <= pgstart && inner.pgend >= pgend
    }

    pub(crate) fn is_subset_of_range(
        &self,
        pgstart: usize,
        pgend: usize,
        guard: &AshmemGuard,
    ) -> bool {
        let inner = self.inner.as_ref(guard);
        inner.pgstart >= pgstart && inner.pgend <= pgend
    }

    pub(crate) fn overlaps_with_range(
        &self,
        pgstart: usize,
        pgend: usize,
        guard: &AshmemGuard,
    ) -> bool {
        self.contains_page(pgstart, guard)
            || self.contains_page(pgend, guard)
            || self.is_subset_of_range(pgstart, pgend, guard)
    }
}

kernel::list::impl_has_list_links! {
    impl HasListLinks<0> for Range { self.lru }
    impl HasListLinks<1> for Range { self.unpinned }
}

kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for Range { untracked; }
    impl ListArcSafe<1> for Range { untracked; }
}

kernel::list::impl_list_item! {
    impl ListItem<0> for Range { using ListLinks; }
    impl ListItem<1> for Range { using ListLinks; }
}

pub(crate) struct Area {
    /// List of page ranges that have been unpinned by `ASHMEM_UNPIN`.
    ///
    /// The ranges are sorted in descending order.
    unpinned_list: List<Range, 1>,
}

impl Drop for Area {
    fn drop(&mut self) {
        let mut guard = AshmemGuard(super::ASHMEM_MUTEX.lock());
        for range in &self.unpinned_list {
            guard.remove_lru(&range);
        }
    }
}

impl Area {
    pub(crate) fn new() -> Self {
        Self {
            unpinned_list: List::new(),
        }
    }

    /// Mark the given range of pages as unpinned so they can be reclaimed.
    ///
    /// The `new_range` argument must be `Some` when calling this method. If this call needs an
    /// allocation, it will take it from the option. Otherwise, the allocation is left in the
    /// option so that the caller can free it after releasing the mutex.
    pub(crate) fn unpin(
        &mut self,
        mut pgstart: usize,
        mut pgend: usize,
        new_range: &mut Option<NewRange<'_>>,
        guard: &mut AshmemGuard,
    ) {
        let mut purged = false;
        let mut cursor = self.unpinned_list.cursor_front();
        while let Some(next) = cursor.peek_next() {
            // Short-circuit: this is our insertion point.
            if next.is_before_page(pgstart, guard) {
                break;
            }

            // If the entire range is already unpinned, just return.
            if next.is_superset_of_range(pgstart, pgend, guard) {
                return;
            }

            if next.overlaps_with_range(pgstart, pgend, guard) {
                pgstart = usize::min(pgstart, next.pgstart(guard));
                pgend = usize::max(pgend, next.pgend(guard));
                purged |= next.purged(guard);
                guard.remove_lru(&next.remove());

                // restart loop
                cursor = self.unpinned_list.cursor_front();
                continue;
            }

            cursor.move_next();
        }

        let new_range = new_range.take().unwrap().init(RangeInner {
            pgstart,
            pgend,
            purged,
        });

        let (range_lru, new_range) = ListArc::<Range, 0>::pair_from_pin_unique::<1>(new_range);
        guard.insert_lru(range_lru);
        cursor.insert(new_range);
    }

    /// Mark the given range of pages as pinned so they can't be reclaimed.
    ///
    /// Returns whether any of the pages have been reclaimed.
    ///
    /// The `new_range` argument must be `Some` when calling this method. If this call needs an
    /// allocation, it will take it from the option. Otherwise, the allocation is left in the
    /// option so that the caller can free it after releasing the mutex.
    pub(crate) fn pin(
        &mut self,
        pgstart: usize,
        pgend: usize,
        new_range: &mut Option<NewRange<'_>>,
        guard: &mut AshmemGuard,
    ) -> bool {
        let mut purged = false;
        let mut cursor = self.unpinned_list.cursor_front();
        while let Some(next) = cursor.peek_next() {
            // moved past last applicable page; we can short circuit
            if next.is_before_page(pgstart, guard) {
                break;
            }

            // The user can ask us to pin pages that span multiple ranges,
            // or to pin pages that aren't even unpinned, so this is messy.
            //
            // Four cases:
            // 1. The requested range subsumes an existing range, so we
            //    just remove the entire matching range.
            // 2. The requested range overlaps the start of an existing
            //    range, so we just update that range.
            // 3. The requested range overlaps the end of an existing
            //    range, so we just update that range.
            // 4. The requested range punches a hole in an existing range,
            //    so we have to update one side of the range and then
            //    create a new range for the other side.
            if next.overlaps_with_range(pgstart, pgend, guard) {
                purged |= next.purged(guard);

                let curr_pgstart = next.pgstart(guard);
                let curr_pgend = next.pgend(guard);

                if next.is_subset_of_range(pgstart, pgend, guard) {
                    // Case #1: Easy. Just nuke the whole thing.
                    let removed = next.remove();
                    guard.remove_lru(&removed);
                    continue;
                } else if curr_pgstart >= pgstart {
                    // Case #2: We overlap from the start, so adjust it.
                    guard.shrink_range(&next, pgend + 1, curr_pgend);
                } else if curr_pgend <= pgend {
                    // Case #3: We overlap from the rear, so adjust it.
                    guard.shrink_range(&next, curr_pgstart, pgstart - 1);
                } else {
                    // Case #4: We eat a chunk out of the middle. A bit
                    // more complicated, we allocate a new range for the
                    // second half and adjust the first chunk's endpoint.
                    guard.shrink_range(&next, curr_pgstart, pgstart - 1);
                    let purged = next.purged(guard);

                    let new_range = new_range.take().unwrap().init(RangeInner {
                        pgstart: pgend + 1,
                        pgend: curr_pgend,
                        purged,
                    });

                    let (range_lru, new_range) =
                        ListArc::<Range, 0>::pair_from_pin_unique::<1>(new_range);
                    guard.insert_lru(range_lru);
                    cursor.insert(new_range);
                    break;
                }
            }

            cursor.move_next();
        }
        purged
    }

    pub(crate) fn range_has_unpinned_page(
        &self,
        pgstart: usize,
        pgend: usize,
        guard: &mut AshmemGuard,
    ) -> bool {
        for range in &self.unpinned_list {
            if range.overlaps_with_range(pgstart, pgend, guard) {
                return true;
            }
        }
        false
    }
}

pub(crate) struct NewRange<'a> {
    pub(crate) file: &'a ShmemFile,
    pub(crate) alloc: UniqueArc<MaybeUninit<Range>>,
}

impl<'a> NewRange<'a> {
    fn init(self, inner: RangeInner) -> Pin<UniqueArc<Range>> {
        let new_range = self.alloc.pin_init_with(pin_init!(Range {
            lru <- ListLinks::new(),
            unpinned <- ListLinks::new(),
            file: self.file.clone(),
            inner: GlobalLockedBy::new(inner),
        }));

        match new_range {
            Ok(new_range) => new_range,
            Err(infallible) => match infallible {},
        }
    }
}

impl AshmemGuard {
    pub(crate) fn free_lru(&mut self, stop_after: usize) -> usize {
        let mut freed = 0;
        while let Some(range) = self.lru_list.pop_back() {
            let start = range.pgstart(self) * PAGE_SIZE;
            let end = (range.pgend(self) + 1) * PAGE_SIZE;
            range.set_purged(self);
            self.remove_lru(&range);
            freed += range.size(self);

            // C ashmem releases the mutex and uses a different mechanism to ensure mutual
            // exclusion with `pin_unpin` operations, but we only hold `ASHMEM_MUTEX` here and in
            // `pin_unpin`, so we don't need to release the mutex. A different mutex is used for
            // all of the other ashmem operations.
            range.file.punch_hole(start, end - start);

            if freed >= stop_after {
                break;
            }

            if super::shrinker_should_stop() {
                break;
            }
        }
        freed
    }
}

impl Shrinker for super::AshmemModule {
    // Our shrinker data is in a global, so we don't need to set the private data.
    type Ptr = ();

    fn count_objects(_: (), _sc: ShrinkControl<'_>) -> CountObjects {
        let count = LRU_COUNT.load(super::Ordering::Relaxed);
        if count == 0 {
            CountObjects::EMPTY
        } else {
            CountObjects::new(count)
        }
    }

    fn scan_objects(_: (), sc: ShrinkControl<'_>) -> ScanObjects {
        if !sc.reclaim_fs_allowed() {
            return ScanObjects::STOP;
        }

        let Some(guard) = super::ASHMEM_MUTEX.try_lock() else {
            return ScanObjects::STOP;
        };
        let mut guard = AshmemGuard(guard);

        let num_freed = guard.free_lru(sc.nr_to_scan());
        ScanObjects::from_count(num_freed)
    }
}

/// Make line below shorter.
type AshmemShrinkerType = Option<ShrinkerRegistration<AshmemModule>>;

kernel::sync::global_lock! {
    // SAFETY: We call `init` as the very first thing in the initialization of this module, so
    // there are no calls to `lock` before `init` is called.
    pub(crate) unsafe(uninit) static ASHMEM_SHRINKER: Mutex<AshmemShrinkerType> = None;
}

pub(crate) fn set_shrinker_enabled(enabled: bool) -> Result<()> {
    let mut shrinker = ASHMEM_SHRINKER.lock();
    if enabled {
        if shrinker.is_none() {
            let mut builder = ShrinkerBuilder::new(c_str!("android-ashmem"))?;
            builder.set_seeks(4 * ashmem_shrinker::DEFAULT_SEEKS);
            *shrinker = Some(builder.register(()));
        }
    } else {
        *shrinker = None;
    }
    Ok(())
}

pub(crate) fn get_shrinker_enabled() -> bool {
    ASHMEM_SHRINKER.lock().is_some()
}

#[cfg(test)]
fn range_test() -> Result {
    fn get_random(max: usize) -> usize {
        let rng = unsafe { kernel::bindings::get_random_u64() };
        (rng % max as u64) as usize
    }

    fn memset(slice: &mut [bool], value: bool) {
        for ptr in slice {
            *ptr = value;
        }
    }

    const SIZE: usize = 16;

    let file = ShmemFile::new(c_str!("test_file"), SIZE * PAGE_SIZE, 0)?;
    let mut area = Area::new();
    let mut unpinned = [false; SIZE];

    let mut new_range = None;
    for _ in 0..SIZE {
        let start = get_random(SIZE);
        let end = get_random(SIZE - start) + start;
        let op = get_random(2) == 0;

        if new_range.is_none() {
            new_range = Some(NewRange {
                file: &file,
                alloc: UniqueArc::new_uninit(GFP_KERNEL)?,
            });
        }
        let mut lock = AshmemGuard(ASHMEM_MUTEX.lock());
        if op {
            pr_err!("Unpinning {start} to {end}.");
            area.unpin(start, end, &mut new_range, &mut lock);
            memset(&mut unpinned[start..=end], true);
        } else {
            pr_err!("Pinning {start} to {end}.");
            area.pin(start, end, &mut new_range, &mut lock);
            memset(&mut unpinned[start..=end], false);
        }

        for item in &area.unpinned_list {
            pr_err!(
                "Seeing range {} to {}.",
                item.pgstart(&lock),
                item.pgend(&lock)
            );
        }

        let mut cursor = area.unpinned_list.cursor_back();
        let mut fail = false;
        for i in 0..SIZE {
            let mut target = false;
            while let Some(prev) = cursor.peek_prev() {
                if prev.pgend(&lock) < i {
                    cursor.move_prev();
                    continue;
                }
                target = prev.pgstart(&lock) <= i;
                break;
            }
            if target != unpinned[i] {
                pr_err!("Mismatch on {i}!");
                fail = true;
            }
        }
        if fail {
            return Err(EINVAL);
        }
    }
    pr_err!("Test completed successfully!");
    Ok(())
}
