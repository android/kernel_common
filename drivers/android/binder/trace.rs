// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

#![allow(unused_variables)]
#![allow(unused_imports)]

use crate::{defs::BinderTransactionDataSg, node::Node, thread::Thread, transaction::Transaction};

use kernel::error::Result;
use kernel::task::{Pid, Task};
use kernel::tracepoint::declare_trace;
use kernel::uapi::flat_binder_object;

use core::ffi::{c_int, c_uint, c_ulong};

#[inline]
pub(crate) fn trace_ioctl(cmd: u32, arg: usize) {}

#[inline]
pub(crate) fn trace_ioctl_done(ret: Result) {}

#[inline]
pub(crate) fn trace_read_done(ret: Result) {}

#[inline]
pub(crate) fn trace_write_done(ret: Result) {}

#[inline]
pub(crate) fn trace_set_priority(thread: &Task, desired_prio: c_int, new_prio: c_int) {}

#[inline]
pub(crate) fn vh_set_priority(t: &Transaction, task: &Task) {}

#[inline]
pub(crate) fn vh_restore_priority(task: &Task) {}

#[inline]
pub(crate) fn trace_wait_for_work(proc_work: bool, transaction_stack: bool, thread_todo: bool) {}

#[inline]
pub(crate) fn trace_transaction(reply: bool, t: &Transaction) {}

#[inline]
pub(crate) fn trace_transaction_received(t: &Transaction) {}

#[inline]
pub(crate) fn trace_transaction_thread_selected(t: &Transaction, th: &Thread) {}

#[inline]
pub(crate) fn trace_transaction_node_send(
    t_debug_id: usize,
    n: &Node,
    orig: &flat_binder_object,
    trans: &flat_binder_object,
) {
}

#[inline]
pub(crate) fn trace_transaction_fd_send(t_debug_id: usize, fd: u32, offset: usize) {}

#[inline]
pub(crate) fn trace_transaction_fd_recv(t_debug_id: usize, fd: u32, offset: usize) {}

#[inline]
pub(crate) fn trace_transaction_alloc_buf(debug_id: usize, data: &BinderTransactionDataSg) {
    let data = data as *const BinderTransactionDataSg;
}

#[inline]
pub(crate) fn trace_transaction_buffer_release(debug_id: usize) {}

#[inline]
pub(crate) fn trace_transaction_failed_buffer_release(debug_id: usize) {}

#[inline]
pub(crate) fn trace_transaction_update_buffer_release(debug_id: usize) {}

#[inline]
pub(crate) fn trace_update_page_range(pid: Pid, allocate: bool, start: usize, end: usize) {}

macro_rules! define_wrapper_lru_page_class {
    ($(fn $name:ident;)*) => {$(
        kernel::macros::paste! {
            #[inline]
            pub(crate) fn [< trace_ $name >](pid: Pid, page_index: usize) {
            }
        }
    )*}
}

define_wrapper_lru_page_class! {
    fn alloc_lru_start;
    fn alloc_lru_end;
    fn free_lru_start;
    fn free_lru_end;
    fn alloc_page_start;
    fn alloc_page_end;
    fn unmap_user_start;
    fn unmap_user_end;
    fn unmap_kernel_start;
    fn unmap_kernel_end;
}

#[inline]
pub(crate) fn trace_command(cmd: u32) {}

#[inline]
pub(crate) fn trace_return(ret: u32) {}
