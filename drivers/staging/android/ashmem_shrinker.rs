// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

#![allow(unreachable_pub, dead_code)]
//! Shrinker for handling memory pressure.
//!
//! C header: [`include/linux/shrinker.h`](srctree/include/linux/shrinker.h)

use kernel::{
    alloc::AllocError,
    bindings, c_str,
    ffi::{c_int, c_long, c_ulong, c_void},
    str::CStr,
    types::ForeignOwnable,
};

use core::{marker::PhantomData, ptr::NonNull};

const SHRINK_STOP: c_ulong = bindings::SHRINK_STOP as c_ulong;
const SHRINK_EMPTY: c_ulong = bindings::SHRINK_EMPTY as c_ulong;

/// The default value for the number of seeks needed to recreate an object.
pub const DEFAULT_SEEKS: u32 = bindings::DEFAULT_SEEKS;

/// An unregistered shrinker.
///
/// This type can be used to modify the settings of the shrinker before it is registered.
///
/// # Invariants
///
/// The `shrinker` pointer references an unregistered shrinker.
pub struct ShrinkerBuilder {
    shrinker: NonNull<bindings::shrinker>,
}

// SAFETY: Moving an unregistered shrinker between threads is okay.
unsafe impl Send for ShrinkerBuilder {}
// SAFETY: An unregistered shrinker is thread safe.
unsafe impl Sync for ShrinkerBuilder {}

impl ShrinkerBuilder {
    /// Create a new shrinker.
    pub fn new(name: &CStr) -> Result<Self, AllocError> {
        // TODO: Support numa/memcg aware shrinkers once list_lru is available.
        let flags = 0;

        // SAFETY: Passing `0` as flags is okay. Using `%s` as the format string is okay when we
        // pass a nul-terminated string as the string for `%s` to print.
        let ptr = unsafe {
            bindings::shrinker_alloc(flags, c_str!("%s").as_char_ptr(), name.as_char_ptr())
        };

        let shrinker = NonNull::new(ptr).ok_or(AllocError)?;

        // INVARIANT: The allocated shrinker is unregistered.
        Ok(Self { shrinker })
    }

    /// Create a new shrinker using format arguments for the name.
    pub fn new_fmt(name: core::fmt::Arguments<'_>) -> Result<Self, AllocError> {
        // TODO: Support numa/memcg aware shrinkers once list_lru is available.
        let flags = 0;

        // SAFETY: Passing `0` as flags is okay. Using `%pA` as the format string is okay when we
        // pass a `fmt::Arguments` as the value to print.
        let ptr = unsafe {
            bindings::shrinker_alloc(
                flags,
                c_str!("%pA").as_char_ptr(),
                &name as *const _ as *const c_void,
            )
        };

        let shrinker = NonNull::new(ptr).ok_or(AllocError)?;

        // INVARIANT: The allocated shrinker is unregistered.
        Ok(Self { shrinker })
    }

    /// Set the number of seeks needed to recreate an object.
    pub fn set_seeks(&mut self, seeks: u32) {
        unsafe { (*self.shrinker.as_ptr()).seeks = seeks as c_int };
    }

    /// Set the batch size for reclaiming on this shrinker.
    pub fn set_batch(&mut self, batch: usize) {
        unsafe { (*self.shrinker.as_ptr()).batch = batch as c_long };
    }

    /// Register the shrinker.
    ///
    /// The provided pointer is used as the private data, and the type `T` determines the callbacks
    /// that the shrinker will use.
    pub fn register<T: Shrinker>(self, private_data: T::Ptr) -> ShrinkerRegistration<T> {
        let shrinker = self.shrinker;
        let ptr = shrinker.as_ptr();

        // The destructor of `self` calls `shrinker_free`, so skip the destructor.
        core::mem::forget(self);

        let private_data_ptr = <T::Ptr as ForeignOwnable>::into_foreign(private_data);

        // SAFETY: We own the private data, so we can assign to it.
        unsafe { (*ptr).private_data = private_data_ptr.cast_mut() };
        // SAFETY: The shrinker is not yet registered, so we can update this field.
        unsafe { (*ptr).count_objects = Some(rust_count_objects::<T>) };
        // SAFETY: The shrinker is not yet registered, so we can update this field.
        unsafe { (*ptr).scan_objects = Some(rust_scan_objects::<T>) };

        // SAFETY: The shrinker is unregistered, so it's safe to register it.
        unsafe { bindings::shrinker_register(ptr) };

        ShrinkerRegistration {
            shrinker,
            _phantom: PhantomData,
        }
    }
}

impl Drop for ShrinkerBuilder {
    fn drop(&mut self) {
        // SAFETY: The shrinker is a valid but unregistered shrinker, and we will not use it
        // anymore.
        unsafe { bindings::shrinker_free(self.shrinker.as_ptr()) };
    }
}

/// A shrinker that is registered with the kernel.
///
/// # Invariants
///
/// The `shrinker` pointer refers to a registered shrinker using `T` as the private data.
pub struct ShrinkerRegistration<T: Shrinker> {
    shrinker: NonNull<bindings::shrinker>,
    _phantom: PhantomData<T::Ptr>,
}

// SAFETY: This allows you to deregister the shrinker from a different thread, which means that
// private data could be dropped from any thread.
unsafe impl<T: Shrinker> Send for ShrinkerRegistration<T> where T::Ptr: Send {}
// SAFETY: The only thing you can do with an immutable reference is access the private data, which
// is okay to access in parallel as the `Shrinker` trait requires the private data to be `Sync`.
unsafe impl<T: Shrinker> Sync for ShrinkerRegistration<T> {}

impl<T: Shrinker> ShrinkerRegistration<T> {
    /// Access the private data in this shrinker.
    pub fn private_data(&self) -> <T::Ptr as ForeignOwnable>::Borrowed<'_> {
        // SAFETY: We own the private data, so we can access it.
        let private = unsafe { (*self.shrinker.as_ptr()).private_data };
        // SAFETY: By the type invariants, the private data is `T`. This access could happen in
        // parallel with a shrinker callback, but that's okay as the `Shrinker` trait ensures that
        // `T::Ptr` is `Sync`.
        unsafe { <T::Ptr as ForeignOwnable>::borrow(private) }
    }
}

impl<T: Shrinker> Drop for ShrinkerRegistration<T> {
    fn drop(&mut self) {
        // SAFETY: We own the private data, so we can access it.
        let private = unsafe { (*self.shrinker.as_ptr()).private_data };
        // SAFETY: We will not access the shrinker after this call.
        unsafe { bindings::shrinker_free(self.shrinker.as_ptr()) };
        // SAFETY: The above call blocked until the completion of any shrinker callbacks, so there
        // are no longer any users of the private data.
        drop(unsafe { <T::Ptr as ForeignOwnable>::from_foreign(private) });
    }
}

/// Callbacks for a shrinker.
pub trait Shrinker {
    /// The pointer type used to store the private data of the shrinker.
    ///
    /// Needs to be `Sync` because the shrinker callback could access this value immutably from
    /// several thread in parallel.
    type Ptr: ForeignOwnable + Sync;

    /// Count the number of freeable items in the cache.
    fn count_objects(
        me: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        sc: ShrinkControl<'_>,
    ) -> CountObjects;

    /// Free some objects in this cache.
    fn scan_objects(
        me: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        sc: ShrinkControl<'_>,
    ) -> ScanObjects;
}

/// How many objects are there in the cache?
///
/// This is used as the return value of [`Shrinker::count_objects`].
pub struct CountObjects {
    inner: c_ulong,
}

impl CountObjects {
    /// Indicates that the number of objects is zero.
    pub const EMPTY: Self = Self {
        inner: SHRINK_EMPTY,
    };

    /// The maximum possible number of freeable objects.
    pub const MAX: Self = Self {
        // The shrinker code assumes that it can multiply this value by two without overflow.
        inner: c_ulong::MAX / 2,
    };

    /// Creates a new `CountObjects` with the given value.
    ///
    /// This should be the number of objects that were actually freed. Objects that were scanned
    /// but not freed should be counted in `nr_scanned` but not here.
    ///
    /// If `count` is zero, then this indicates that the real count is unknown. Use
    /// `CountObjects::EMPTY` to indicate that the shrinker is empty.
    pub fn new(count: usize) -> Self {
        if count > Self::MAX.inner as usize {
            return Self::MAX;
        }

        Self {
            inner: count as c_ulong,
        }
    }
}

/// How many objects were freed?
///
/// This is used as the return value of [`Shrinker::scan_objects`].
pub struct ScanObjects {
    inner: c_ulong,
}

impl ScanObjects {
    /// Indicates that the shrinker should stop trying to free objects from this cache due to
    /// potential deadlocks.
    pub const STOP: Self = Self { inner: SHRINK_STOP };

    /// The maximum possible number of freeable objects.
    pub const MAX: Self = Self {
        inner: SHRINK_STOP - 1,
    };

    /// Creates a new `CountObjects` with the given value.
    pub fn from_count(count: usize) -> Self {
        if count > Self::MAX.inner as usize {
            return Self::MAX;
        }

        Self {
            inner: count as c_ulong,
        }
    }
}

/// This struct is used to pass information from page reclaim to the shrinkers.
///
/// # Invariants
///
/// `ptr` has exclusive access to a valid `struct shrink_control`.
pub struct ShrinkControl<'a> {
    ptr: NonNull<bindings::shrink_control>,
    _phantom: PhantomData<&'a bindings::shrink_control>,
}

impl<'a> ShrinkControl<'a> {
    /// Create a `ShrinkControl` from a raw pointer.
    ///
    /// # Safety
    ///
    /// The pointer should point at a valid `shrink_control` for the duration of 'a.
    pub unsafe fn from_raw(ptr: *mut bindings::shrink_control) -> Self {
        Self {
            // SAFETY: Caller promises that this pointer is valid.
            ptr: unsafe { NonNull::new_unchecked(ptr) },
            _phantom: PhantomData,
        }
    }

    /// Determines whether it is safe to call into filesystem code.
    pub fn reclaim_fs_allowed(&self) -> bool {
        // SAFETY: Okay by type invariants.
        let mask = unsafe { (*self.ptr.as_ptr()).gfp_mask };

        (mask & bindings::__GFP_FS) != 0
    }

    /// Determines whether it is safe to call into IO code.
    pub fn reclaim_io_allowed(&self) -> bool {
        // SAFETY: Okay by type invariants.
        let mask = unsafe { (*self.ptr.as_ptr()).gfp_mask };

        (mask & bindings::__GFP_IO) != 0
    }

    /// Returns the number of objects that `scan_objects` should try to reclaim.
    pub fn nr_to_scan(&self) -> usize {
        // SAFETY: Okay by type invariants.
        unsafe { (*self.ptr.as_ptr()).nr_to_scan as usize }
    }

    /// The callback should set this value to the number of objects inspected by the shrinker.
    pub fn set_nr_scanned(&mut self, val: usize) {
        // SAFETY: Okay by type invariants.
        unsafe { (*self.ptr.as_ptr()).nr_scanned = val as c_ulong };
    }
}

unsafe extern "C" fn rust_count_objects<T: Shrinker>(
    shrink: *mut bindings::shrinker,
    sc: *mut bindings::shrink_control,
) -> c_ulong {
    // SAFETY: We own the private data, so we can access it.
    let private = unsafe { (*shrink).private_data };
    // SAFETY: This function is only used with shrinkers where `T` is the type of the private data.
    let private = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
    // SAFETY: The caller passes a valid `sc` pointer.
    let sc = unsafe { ShrinkControl::from_raw(sc) };

    let ret = T::count_objects(private, sc);
    ret.inner
}

unsafe extern "C" fn rust_scan_objects<T: Shrinker>(
    shrink: *mut bindings::shrinker,
    sc: *mut bindings::shrink_control,
) -> c_ulong {
    // SAFETY: We own the private data, so we can access it.
    let private = unsafe { (*shrink).private_data };
    // SAFETY: This function is only used with shrinkers where `T` is the type of the private data.
    let private = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
    // SAFETY: The caller passes a valid `sc` pointer.
    let sc = unsafe { ShrinkControl::from_raw(sc) };

    let ret = T::scan_objects(private, sc);
    ret.inner
}
