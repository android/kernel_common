// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Provides knobs for Rust ashmem.

use crate::{ashmem_range, IGNORE_UNSET_PROT_EXEC, IGNORE_UNSET_PROT_READ};
use core::{marker::PhantomData, sync::atomic::Ordering};
use kernel::{
    c_str,
    error::to_result,
    fs::File,
    miscdevice::{IovIter, Kiocb, MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    prelude::*,
};

fn kstrtobool(kstr: &CStr) -> Result<bool> {
    let mut res = false;
    to_result(unsafe { kernel::bindings::kstrtobool(kstr.as_char_ptr(), &mut res) })?;
    Ok(res)
}

pub(crate) trait AshmemToggle {
    const NAME: &'static CStr;
    fn set(enabled: bool) -> Result<()>;
    fn get() -> bool;
}

pub(crate) struct AshmemToggleMisc<T>(PhantomData<T>);

impl<T: AshmemToggle> AshmemToggleMisc<T> {
    pub(crate) fn new() -> Result<Pin<KBox<MiscDeviceRegistration<AshmemToggleMisc<T>>>>> {
        KBox::pin_init(
            MiscDeviceRegistration::register(MiscDeviceOptions { name: T::NAME }),
            GFP_KERNEL,
        )
    }
}

#[vtable]
impl<T: AshmemToggle> MiscDevice for AshmemToggleMisc<T> {
    type Ptr = ();
    fn open(_: &File, _: &MiscDeviceRegistration<Self>) -> Result<()> {
        Ok(())
    }
    fn read_iter(mut kiocb: Kiocb<'_, Self::Ptr>, iov: &mut IovIter) -> Result<usize> {
        if kiocb.ki_pos() != 0 {
            return Ok(0);
        }

        let data = match T::get() {
            false => b"0\n",
            true => b"1\n",
        };

        // You better give me a buffer with space for at least two bytes.
        iov.copy_to_iter(data)?;
        *kiocb.ki_pos_mut() = 2;
        Ok(2)
    }
    fn write_iter(_kiocb: Kiocb<'_, Self::Ptr>, iov: &mut IovIter) -> Result<usize> {
        let mut data = [0; 16];
        let len = iov.copy_from_iter(&mut data[..15]);
        data[len] = 0;
        let data = CStr::from_bytes_with_nul(&data[..len + 1])?;
        T::set(kstrtobool(data)?)?;
        Ok(len)
    }
}

pub(crate) struct AshmemToggleShrinker;

impl AshmemToggle for AshmemToggleShrinker {
    const NAME: &'static CStr = c_str!("ashmem_unpinning_enable");
    fn set(enabled: bool) -> Result<()> {
        ashmem_range::set_shrinker_enabled(enabled)
    }
    fn get() -> bool {
        ashmem_range::get_shrinker_enabled()
    }
}

pub(crate) struct AshmemToggleRead;

impl AshmemToggle for AshmemToggleRead {
    const NAME: &'static CStr = c_str!("ashmem_ignore_unset_prot_read");
    fn set(enabled: bool) -> Result<()> {
        IGNORE_UNSET_PROT_READ.store(enabled, Ordering::Relaxed);
        Ok(())
    }
    fn get() -> bool {
        IGNORE_UNSET_PROT_READ.load(Ordering::Relaxed)
    }
}

pub(crate) struct AshmemToggleExec;

impl AshmemToggle for AshmemToggleExec {
    const NAME: &'static CStr = c_str!("ashmem_ignore_unset_prot_exec");
    fn set(enabled: bool) -> Result<()> {
        IGNORE_UNSET_PROT_EXEC.store(enabled, Ordering::Relaxed);
        Ok(())
    }
    fn get() -> bool {
        IGNORE_UNSET_PROT_EXEC.load(Ordering::Relaxed)
    }
}
