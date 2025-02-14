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
    bindings, c_str,
    error::Result,
    fs::File,
    ioctl::_IOC_SIZE,
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    mm::virt::{flags as vma_flags, VmAreaNew},
    page::page_align,
    prelude::*,
    sync::{new_mutex, Mutex},
    task::Task,
    uaccess::{UserSlice, UserSliceReader, UserSliceWriter},
};

const ASHMEM_NAME_LEN: usize = bindings::ASHMEM_NAME_LEN as usize;
const ASHMEM_FULL_NAME_LEN: usize = bindings::ASHMEM_FULL_NAME_LEN as usize;
const ASHMEM_NAME_PREFIX_LEN: usize = bindings::ASHMEM_NAME_PREFIX_LEN as usize;
const ASHMEM_NAME_PREFIX: [u8; ASHMEM_NAME_PREFIX_LEN] = *b"dev/ashmem/";

const PROT_READ: usize = bindings::PROT_READ as usize;
const PROT_EXEC: usize = bindings::PROT_EXEC as usize;
const PROT_WRITE: usize = bindings::PROT_WRITE as usize;
const PROT_MASK: usize = PROT_EXEC | PROT_READ | PROT_WRITE;

mod shmem;
use shmem::ShmemFile;

/// Does PROT_READ imply PROT_EXEC for this task?
fn read_implies_exec(task: &Task) -> bool {
    // SAFETY: Always safe to read.
    let personality = unsafe { (*task.as_ptr()).personality };
    (personality & bindings::READ_IMPLIES_EXEC) != 0
}

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
        // SAFETY: Called once since this is the module initializer.
        unsafe { shmem::SHMEM_FOPS_ONCE.init() };

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
struct Ashmem {
    #[pin]
    inner: Mutex<AshmemInner>,
}

struct AshmemInner {
    size: usize,
    prot_mask: usize,
    /// If set, then this holds the ashmem name without the dev/ashmem/ prefix. No zero terminator.
    name: Option<Vec<u8>>,
    file: Option<ShmemFile>,
}

#[vtable]
impl MiscDevice for Ashmem {
    type Ptr = Pin<Box<Self>>;

    fn open(_: &File, _: &MiscDeviceRegistration<Ashmem>) -> Result<Pin<Box<Self>>> {
        Box::try_pin_init(
            try_pin_init! {
                Ashmem {
                    inner <- new_mutex!(AshmemInner {
                        size: 0,
                        prot_mask: PROT_MASK,
                        name: None,
                        file: None,
                    }),
                }
            },
            GFP_KERNEL,
        )
    }

    fn mmap(me: Pin<&Ashmem>, _file: &File, vma: &VmAreaNew) -> Result<()> {
        let asma = &mut *me.inner.lock();

        // User needs to SET_SIZE before mapping.
        if asma.size == 0 {
            return Err(EINVAL);
        }

        // Requested mapping size larger than object size.
        if vma.end() - vma.start() > page_align(asma.size) {
            return Err(EINVAL);
        }

        if asma.prot_mask & PROT_WRITE == 0 {
            vma.try_clear_maywrite().map_err(|_| EPERM)?;
        }
        if asma.prot_mask & PROT_EXEC == 0 {
            vma.try_clear_mayexec().map_err(|_| EPERM)?;
        }
        if asma.prot_mask & PROT_READ == 0 {
            vma.try_clear_mayread().map_err(|_| EPERM)?;
        }

        let file = match asma.file.as_ref() {
            Some(file) => file,
            None => {
                let mut name_buffer = [0u8; ASHMEM_FULL_NAME_LEN];
                let name = asma.full_name(&mut name_buffer);
                asma.file
                    .insert(ShmemFile::new(name, asma.size, vma.flags())?)
            }
        };

        if vma.flags() & vma_flags::SHARED != 0 {
            // We're really using this just to set vm_ops to `shmem_anon_vm_ops`. Anything else it
            // does is undone by the call to `set_file` below.
            shmem::zero_setup(vma)?;
        } else {
            shmem::vma_set_anonymous(vma);
        }

        shmem::set_file(vma, file.file());
        Ok(())
    }

    fn ioctl(me: Pin<&Ashmem>, _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        let size = _IOC_SIZE(cmd);
        match cmd {
            bindings::ASHMEM_SET_NAME => me.set_name(UserSlice::new(arg, size).reader()),
            bindings::ASHMEM_GET_NAME => me.get_name(UserSlice::new(arg, size).writer()),
            bindings::ASHMEM_SET_SIZE => me.set_size(arg),
            bindings::ASHMEM_GET_SIZE => me.get_size(),
            bindings::ASHMEM_SET_PROT_MASK => me.set_prot_mask(arg),
            bindings::ASHMEM_GET_PROT_MASK => me.get_prot_mask(),
            _ => Err(EINVAL),
        }
    }

    #[cfg(CONFIG_COMPAT)]
    fn compat_ioctl(me: Pin<&Ashmem>, file: &File, compat_cmd: u32, arg: usize) -> Result<isize> {
        let cmd = match compat_cmd {
            bindings::COMPAT_ASHMEM_SET_SIZE => bindings::ASHMEM_SET_SIZE,
            bindings::COMPAT_ASHMEM_SET_PROT_MASK => bindings::ASHMEM_SET_PROT_MASK,
            _ => compat_cmd,
        };
        Self::ioctl(me, file, cmd, arg)
    }
}

impl Ashmem {
    fn set_name(&self, mut reader: UserSliceReader) -> Result<isize> {
        let mut local_name = [0u8; ASHMEM_NAME_LEN];
        reader.read_slice(&mut local_name)?;

        // Find the zero terminator. If the zero terminator is missing, the string is truncated to
        // `ASHMEM_NAME_LEN-1` so that `get_name` can return it and has enough space to add a zero
        // terminator.
        let len = local_name
            .iter()
            .position(|&c| c == 0)
            .unwrap_or(local_name.len() - 1);

        let mut v = Vec::with_capacity(len, GFP_KERNEL)?;
        v.extend_from_slice(&local_name[..len], GFP_KERNEL)?;

        let mut asma = self.inner.lock();
        if asma.file.is_some() {
            return Err(EINVAL);
        }
        asma.name = Some(v);
        Ok(0)
    }

    fn get_name(&self, mut writer: UserSliceWriter) -> Result<isize> {
        let mut local_name = [0u8; ASHMEM_NAME_LEN];
        let asma = self.inner.lock();
        let name = asma.name.as_deref().unwrap_or(b"dev/ashmem");
        let len = name.len();
        let len_with_nul = len + 1;
        if local_name.len() <= len_with_nul {
            // This shouldn't happen in practice since `set_name` will refuse to store a string
            // that is too long.
            return Err(EINVAL);
        }
        local_name[..len].copy_from_slice(name);
        local_name[len_with_nul] = 0;
        drop(asma);

        writer.write_slice(&local_name[..len_with_nul])?;
        Ok(0)
    }

    fn set_size(&self, size: usize) -> Result<isize> {
        let mut asma = self.inner.lock();
        if asma.file.is_some() {
            return Err(EINVAL);
        }
        asma.size = size;
        Ok(0)
    }

    fn get_size(&self) -> Result<isize> {
        Ok(self.inner.lock().size as isize)
    }

    fn set_prot_mask(&self, mut prot: usize) -> Result<isize> {
        let mut asma = self.inner.lock();

        if (prot & PROT_READ != 0) && read_implies_exec(current!()) {
            prot |= PROT_EXEC;
        }

        // The user can only remove, not add, protection bits.
        if (asma.prot_mask & prot) != prot {
            return Err(EINVAL);
        }

        asma.prot_mask = prot;
        Ok(0)
    }

    fn get_prot_mask(&self) -> Result<isize> {
        Ok(self.inner.lock().prot_mask as isize)
    }
}

impl AshmemInner {
    /// Get the full name.
    ///
    /// If the name is `Some(name)`, then this returns `dev/ashmem/name\0`.
    ///
    /// If the name is `None`, then this returns `dev/ashmem\0`.
    fn full_name<'name>(&self, name: &'name mut [u8; ASHMEM_FULL_NAME_LEN]) -> &'name CStr {
        name[..ASHMEM_NAME_PREFIX_LEN].copy_from_slice(&ASHMEM_NAME_PREFIX);
        if let Some(set_name) = self.name.as_deref() {
            name[ASHMEM_NAME_PREFIX_LEN..][..set_name.len()].copy_from_slice(set_name);
        } else {
            // Remove last slash if no name set.
            name[ASHMEM_NAME_PREFIX_LEN - 1] = 0;
        }
        name[ASHMEM_FULL_NAME_LEN - 1] = 0;

        // This unwrap only fails if there's no nul-byte, but we just added one at the end above.
        let len_with_nul = name
            .iter()
            .position(|&c| c == 0)
            .map(|len| len + 1)
            .unwrap();

        // This unwrap fails if the last byte is not a nul-byte, or if there are any nul-bytes
        // before the last byte. Neither of those are possible here since `len_with_nul` is the
        // index of the first nul-byte in `name`.
        CStr::from_bytes_with_nul(&name[..len_with_nul]).unwrap()
    }
}
