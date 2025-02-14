// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Anonymous Shared Memory Subsystem for Android.
//!
//! The ashmem subsystem is a new shared memory allocator, similar to POSIX SHM but with different
//! behavior and sporting a simpler file-based API.
//!
//! It is, in theory, a good memory allocator for low-memory devices, because it can discard shared
//! memory units when under memory pressure.

use core::pin::Pin;
use kernel::{
    c_str,
    error::Result,
    fs::File,
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    prelude::*,
};

module! {
    type: AshmemModule,
    name: "ashmem_rust",
    author: "Alice Ryhl",
    description: "Anonymous Shared Memory Subsystem",
    license: "GPL",
}

struct AshmemModule {
    _misc: Pin<Box<MiscDeviceRegistration<Ashmem>>>,
}

impl kernel::Module for AshmemModule {
    fn init(_module: &'static kernel::ThisModule) -> Result<Self> {
        pr_info!("Using Rust implementation.");

        Ok(Self {
            _misc: Box::pin_init(
                MiscDeviceRegistration::register(MiscDeviceOptions {
                    name: c_str!("ashmem"),
                }),
                GFP_KERNEL,
            )?,
        })
    }
}

/// Represents an open ashmem file.
#[pin_data]
struct Ashmem {}

#[vtable]
impl MiscDevice for Ashmem {
    type Ptr = Pin<Box<Self>>;

    fn open(_: &File, _: &MiscDeviceRegistration<Ashmem>) -> Result<Pin<Box<Self>>> {
        Box::try_pin_init(Ashmem {}, GFP_KERNEL)
    }
}
