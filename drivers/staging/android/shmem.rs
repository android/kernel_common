// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Safe rust abstraction around a shmem file for use by ashmem.

use kernel::{
    bindings,
    error::{from_err_ptr, to_result, Result},
    ffi::{c_int, c_ulong},
    fs::file::{File, LocalFile},
    miscdevice::{loff_t, IovIter},
    mm::virt::{vm_flags_t, VmAreaNew},
    prelude::*,
    str::CStr,
    types::ARef,
};

use core::{
    cell::UnsafeCell,
    ptr::{addr_of_mut, NonNull},
};

/// # Safety
///
/// Caller must ensure that access to the file position is properly synchronized.
pub(crate) unsafe fn file_get_fpos(file: &LocalFile) -> loff_t {
    // SAFETY: Caller ensures that this is okay.
    unsafe { (*file.as_ptr()).f_pos }
}

/// # Safety
///
/// Caller must ensure that access to the file position is properly synchronized.
pub(crate) unsafe fn file_set_fpos(file: &LocalFile, pos: loff_t) {
    // SAFETY: Caller ensures that this is okay.
    unsafe { (*file.as_ptr()).f_pos = pos };
}

pub(crate) fn vma_set_anonymous(vma: &VmAreaNew) {
    // SAFETY: The `VmAreaNew` type is only used when the vma is being set up, so this operation is
    // safe.
    unsafe { (*vma.as_ptr()).vm_ops = core::ptr::null_mut() };
}

/// Wrapper around a file that is known to be a shmem file.
#[derive(Clone)]
pub(crate) struct ShmemFile {
    inner: ARef<File>,
}

impl ShmemFile {
    /// Create a shmem file for use by ashmem.
    ///
    /// This sets up the file with the exact configuration that ashmem needs.
    pub(crate) fn new(name: &CStr, size: usize, flags: vm_flags_t) -> Result<Self> {
        // SAFETY: The name is a nul-terminated string.
        let vmfile = from_err_ptr(unsafe {
            bindings::shmem_file_setup(name.as_char_ptr(), size as _, flags)
        })?;

        // SAFETY: The call to `shmem_file_setup` was successful, so `vmfile` is a valid pointer to
        // a file and we can transfer ownership of the refcount it created to an `ARef<File>`.
        let vmfile = unsafe { ARef::<File>::from_raw(NonNull::new_unchecked(vmfile.cast())) };

        // The C driver sets the FMODE_LSEEK bit in `f_mode` here. However, that is not necessary
        // anymore. It was added to the C driver in commit 97fbfef6bd59 ("staging: android: ashmem:
        // lseek failed due to no FMODE_LSEEK.") since they started using the VFS implementation of
        // lseek rather than a custom hook, and the VFS version actually checks the permissions.
        //
        // However, commit e7478158e137 ("fs: clear or set FMODE_LSEEK based on llseek function")
        // has since made it so that if lseek is implemented, then FMODE_LSEEK will be set on
        // pseudo-files by default. Since llseek is implemented on shmem files, we no longer need
        // to set FMODE_LSEEK.

        set_inode_lockdep_class(&vmfile);

        // SAFETY: We just created the file and have not yet published it, so nobody else is
        // looking at this field yet.
        unsafe { (*vmfile.as_ptr()).f_op = get_shmem_fops((*vmfile.as_ptr()).f_op) };

        Ok(Self { inner: vmfile })
    }

    pub(crate) fn file(&self) -> &File {
        &self.inner
    }

    pub(crate) fn vfs_llseek(&self, offset: loff_t, whence: c_int) -> Result<loff_t> {
        // SAFETY: Just an FFI call. The file is valid.
        let ret = unsafe { bindings::vfs_llseek(self.inner.as_ptr(), offset, whence) };

        if ret < 0 {
            Err(Error::from_errno(ret as i32))
        } else {
            Ok(ret)
        }
    }

    pub(crate) fn vfs_iter_read(&self, iov: &mut IovIter, pos: &mut loff_t) -> Result<loff_t> {
        // SAFETY: Just an FFI call. The file and iov is valid.
        let ret = unsafe { bindings::vfs_iter_read(self.inner.as_ptr(), iov.as_raw(), pos, 0) };

        if ret < 0 {
            Err(Error::from_errno(ret as i32))
        } else {
            Ok(ret as loff_t)
        }
    }

    pub(crate) fn punch_hole(&self, start: usize, len: usize) {
        use kernel::bindings::{FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE};

        let f = self.inner.as_ptr();
        // SAFETY: f_op of a file is immutable, so okay to read.
        let fallocate = unsafe { (*(*f).f_op).fallocate };

        if let Some(fallocate) = fallocate {
            unsafe {
                fallocate(
                    f,
                    (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE) as _,
                    start as _,
                    len as _,
                )
            };
        }
    }

    pub(crate) fn inode_ino(&self) -> usize {
        // SAFETY: Accessing the ino is always okay.
        unsafe { (*(*self.inner.as_ptr()).f_inode).i_ino as usize }
    }
}

/// Fix the lockdep class of the shmem inode.
///
/// A separate lockdep class for the backing shmem inodes to resolve the lockdep warning about the
/// race between kswapd taking fs_reclaim before inode_lock and write syscall taking inode_lock and
/// then fs_reclaim. Note that such race is impossible because ashmem does not support write
/// syscalls operating on the backing shmem.
fn set_inode_lockdep_class(vmfile: &File) {
    // SAFETY: This sets the lockdep class correctly.
    unsafe {
        let inode = (*vmfile.as_ptr()).f_inode;
        let lock = addr_of_mut!((*inode).i_rwsem);
        bindings::lockdep_set_class_rwsem(
            lock,
            kernel::static_lock_class!().as_ptr(),
            kernel::c_str!("backing_shmem_inode_class").as_char_ptr(),
        )
    }
}

pub(crate) fn zero_setup(vma: &VmAreaNew) -> Result<()> {
    // SAFETY: The `VmAreaNew` type is only used when the vma is being set up, so we can set up the
    // vma.
    to_result(unsafe { bindings::shmem_zero_setup(vma.as_ptr()) })
}

pub(crate) fn set_file(vma: &VmAreaNew, file: &File) {
    let file = ARef::from(file);
    // SAFETY: We're setting up the vma, so we can read the file pointer.
    let old_file = unsafe { (*vma.as_ptr()).vm_file };

    // INVARIANT: This transfers ownership of the refcount we just created to the vma.
    //
    // SAFETY: We're setting up the vma, so we can write to the file pointer.
    unsafe { (*vma.as_ptr()).vm_file = ARef::into_raw(file).as_ptr().cast() };

    if let Some(old_file) = NonNull::new(old_file) {
        // SAFETY: We took ownership of the file refcount from the vma, so we can drop it.
        drop(unsafe { ARef::<File>::from_raw(old_file.cast()) });
    }
}

// Used to synchronize the initialization of `VMFILE_FOPS`.
//
// INVARIANT: Once `SHMEM_FOPS_ONCE` becomes true, `VMFILE_FOPS` is permanently immutable.
kernel::sync::global_lock! {
    // SAFETY: We call `init` as the very first thing in the initialization of this module, so
    // there are no calls to `lock` before `init` is called.
    pub(super) unsafe(uninit) static SHMEM_FOPS_ONCE: Mutex<bool> = false;
}

/// # Safety
///
/// Must only be used with the fops of a shmem file.
unsafe fn get_shmem_fops(
    shmem_fops: *const bindings::file_operations,
) -> &'static bindings::file_operations {
    struct FopsHelper {
        inner: UnsafeCell<bindings::file_operations>,
    }
    unsafe impl Sync for FopsHelper {}

    static VMFILE_FOPS: FopsHelper = FopsHelper {
        // SAFETY: All zeros is valid for `struct file_operations`.
        inner: UnsafeCell::new(unsafe { core::mem::zeroed() }),
    };

    let fops_ptr = VMFILE_FOPS.inner.get();

    let mut once_guard = SHMEM_FOPS_ONCE.lock();
    if !*once_guard {
        // SAFETY: This points at the file operations of an existing file, so the contents must be
        // immutable.
        let mut new_fops = unsafe { *shmem_fops };
        new_fops.mmap = Some(ashmem_vmfile_mmap);
        new_fops.get_unmapped_area = Some(ashmem_vmfile_get_unmapped_area);
        // SAFETY: We hold the `SHMEM_FOPS_ONCE` guard, so there are no other writers. The value of
        // `SHMEM_FOPS_ONCE` is false, so there are no readers either.
        unsafe { *fops_ptr = new_fops };
        *once_guard = true;
    }
    drop(once_guard);

    // SAFETY: The value of `SHMEM_FOPS_ONCE` is true, so `VMFILE_FOPS` is never going to change
    // again.
    unsafe { &*fops_ptr }
}

extern "C" fn ashmem_vmfile_mmap(
    _file: *mut bindings::file,
    _vma: *mut bindings::vm_area_struct,
) -> c_int {
    EPERM.to_errno()
}

unsafe extern "C" fn ashmem_vmfile_get_unmapped_area(
    file: *mut bindings::file,
    addr: c_ulong,
    len: c_ulong,
    pgoff: c_ulong,
    flags: c_ulong,
) -> c_ulong {
    // SAFETY: The `mm` of current does not change, so it is safe to access.
    let mm = unsafe { (*bindings::get_current()).mm };
    // SAFETY: This calls the right get_unmapped_area for a shmem.
    unsafe { bindings::mm_get_unmapped_area(mm, file, addr, len, pgoff, flags) }
}
