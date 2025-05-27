// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

use crate::{defs::BinderTransactionDataSg, node::Node, thread::Thread, transaction::Transaction};

use kernel::bindings::{
    binder_transaction_data_sg, flat_binder_object, rust_binder_node, rust_binder_thread,
    rust_binder_transaction, task_struct,
};
use kernel::error::Result;
use kernel::ffi::{c_int, c_uint, c_ulong};
use kernel::task::{Pid, Task};
use kernel::tracepoint::declare_trace;
use kernel::uapi;

declare_trace! {
    unsafe fn rust_binder_ioctl(cmd: c_uint, arg: c_ulong);
    unsafe fn rust_binder_ioctl_done(ret: c_int);
    unsafe fn rust_binder_read_done(ret: c_int);
    unsafe fn rust_binder_write_done(ret: c_int);
    unsafe fn rust_binder_set_priority(thread: *mut task_struct, desired_prio: c_int, new_prio: c_int);
    unsafe fn android_vh_rust_binder_set_priority(t: rust_binder_transaction, task: *mut task_struct);
    unsafe fn android_vh_rust_binder_restore_priority(task: *mut task_struct);
    unsafe fn rust_binder_wait_for_work(proc_work: bool, transaction_stack: bool, thread_todo: bool);
    unsafe fn rust_binder_transaction(reply: bool, t: rust_binder_transaction);
    unsafe fn rust_binder_transaction_received(t: rust_binder_transaction);
    unsafe fn rust_binder_transaction_thread_selected(t: rust_binder_transaction, thread: rust_binder_thread);
    unsafe fn rust_binder_transaction_node_send(t_debug_id: c_int, n: rust_binder_node,
                                                orig: *const flat_binder_object,
                                                trans: *const flat_binder_object);
    unsafe fn rust_binder_transaction_fd_send(t_debug_id: c_int, fd: c_int, offset: usize);
    unsafe fn rust_binder_transaction_fd_recv(t_debug_id: c_int, fd: c_int, offset: usize);
    unsafe fn rust_binder_transaction_alloc_buf(debug_id: c_int, data: *const binder_transaction_data_sg);
    unsafe fn rust_binder_transaction_buffer_release(debug_id: c_int);
    unsafe fn rust_binder_transaction_failed_buffer_release(debug_id: c_int);
    unsafe fn rust_binder_transaction_update_buffer_release(debug_id: c_int);
    unsafe fn rust_binder_update_page_range(pid: c_int, allocate: bool, start: usize, end: usize);
    unsafe fn rust_binder_alloc_lru_start(pid: c_int, page_index: usize);
    unsafe fn rust_binder_alloc_lru_end(pid: c_int, page_index: usize);
    unsafe fn rust_binder_free_lru_start(pid: c_int, page_index: usize);
    unsafe fn rust_binder_free_lru_end(pid: c_int, page_index: usize);
    unsafe fn rust_binder_alloc_page_start(pid: c_int, page_index: usize);
    unsafe fn rust_binder_alloc_page_end(pid: c_int, page_index: usize);
    unsafe fn rust_binder_unmap_user_start(pid: c_int, page_index: usize);
    unsafe fn rust_binder_unmap_user_end(pid: c_int, page_index: usize);
    unsafe fn rust_binder_unmap_kernel_start(pid: c_int, page_index: usize);
    unsafe fn rust_binder_unmap_kernel_end(pid: c_int, page_index: usize);
    unsafe fn rust_binder_command(cmd: u32);
    unsafe fn rust_binder_return(ret: u32);
}

#[inline]
fn raw_transaction(t: &Transaction) -> rust_binder_transaction {
    t as *const Transaction as rust_binder_transaction
}

#[inline]
fn raw_thread(t: &Thread) -> rust_binder_thread {
    t as *const Thread as rust_binder_thread
}

#[inline]
fn raw_node(n: &Node) -> rust_binder_node {
    n as *const Node as rust_binder_node
}

#[inline]
fn to_errno(ret: Result) -> i32 {
    match ret {
        Ok(()) => 0,
        Err(err) => err.to_errno(),
    }
}

#[inline]
pub(crate) fn trace_ioctl(cmd: u32, arg: usize) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_ioctl(cmd, arg as c_ulong) }
}

#[inline]
pub(crate) fn trace_ioctl_done(ret: Result) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_ioctl_done(to_errno(ret)) }
}

#[inline]
pub(crate) fn trace_read_done(ret: Result) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_read_done(to_errno(ret)) }
}

#[inline]
pub(crate) fn trace_write_done(ret: Result) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_write_done(to_errno(ret)) }
}

#[inline]
pub(crate) fn trace_set_priority(thread: &Task, desired_prio: c_int, new_prio: c_int) {
    // SAFETY: The pointer to the task is valid for the duration of this call.
    unsafe { rust_binder_set_priority(thread.as_ptr(), desired_prio, new_prio) }
}

#[inline]
pub(crate) fn vh_set_priority(t: &Transaction, task: &Task) {
    // SAFETY: The pointers to `t` and `task` are valid.
    unsafe { android_vh_rust_binder_set_priority(raw_transaction(t), task.as_ptr()) }
}

#[inline]
pub(crate) fn vh_restore_priority(task: &Task) {
    // SAFETY: The pointer to `task` is valid.
    unsafe { android_vh_rust_binder_restore_priority(task.as_ptr()) }
}

#[inline]
pub(crate) fn trace_wait_for_work(proc_work: bool, transaction_stack: bool, thread_todo: bool) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_wait_for_work(proc_work, transaction_stack, thread_todo) }
}

#[inline]
pub(crate) fn trace_transaction(reply: bool, t: &Transaction) {
    // SAFETY: The raw transaction is valid for the duration of this call.
    unsafe { rust_binder_transaction(reply, raw_transaction(t)) }
}

#[inline]
pub(crate) fn trace_transaction_received(t: &Transaction) {
    // SAFETY: The raw transaction is valid for the duration of this call.
    unsafe { rust_binder_transaction_received(raw_transaction(t)) }
}

#[inline]
pub(crate) fn trace_transaction_thread_selected(t: &Transaction, th: &Thread) {
    // SAFETY: The raw transaction is valid for the duration of this call.
    unsafe { rust_binder_transaction_thread_selected(raw_transaction(t), raw_thread(th)) }
}

#[inline]
pub(crate) fn trace_transaction_node_send(
    t_debug_id: usize,
    n: &Node,
    orig: &uapi::flat_binder_object,
    trans: &uapi::flat_binder_object,
) {
    // CAST: Types are identical.
    let orig = orig as *const uapi::flat_binder_object as *const flat_binder_object;
    // CAST: Types are identical.
    let trans = trans as *const uapi::flat_binder_object as *const flat_binder_object;

    // SAFETY: The pointers are valid for the duration of this call.
    unsafe { rust_binder_transaction_node_send(t_debug_id as c_int, raw_node(n), orig, trans) }
}

#[inline]
pub(crate) fn trace_transaction_fd_send(t_debug_id: usize, fd: u32, offset: usize) {
    // SAFETY: This function is always safe to call.
    unsafe { rust_binder_transaction_fd_send(t_debug_id as c_int, fd as c_int, offset) }
}

#[inline]
pub(crate) fn trace_transaction_fd_recv(t_debug_id: usize, fd: u32, offset: usize) {
    // SAFETY: This function is always safe to call.
    unsafe { rust_binder_transaction_fd_recv(t_debug_id as c_int, fd as c_int, offset) }
}

#[inline]
pub(crate) fn trace_transaction_alloc_buf(debug_id: usize, data: &BinderTransactionDataSg) {
    let data = data as *const BinderTransactionDataSg;
    // SAFETY: The `data` pointer is valid.
    unsafe { rust_binder_transaction_alloc_buf(debug_id as c_int, data.cast()) }
}

#[inline]
pub(crate) fn trace_transaction_buffer_release(debug_id: usize) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_transaction_buffer_release(debug_id as c_int) }
}

#[inline]
pub(crate) fn trace_transaction_failed_buffer_release(debug_id: usize) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_transaction_failed_buffer_release(debug_id as c_int) }
}

#[inline]
pub(crate) fn trace_transaction_update_buffer_release(debug_id: usize) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_transaction_update_buffer_release(debug_id as c_int) }
}

#[inline]
pub(crate) fn trace_update_page_range(pid: Pid, allocate: bool, start: usize, end: usize) {
    // SAFETY: Always safe to call.
    unsafe { rust_binder_update_page_range(pid as c_int, allocate, start, end) }
}

macro_rules! define_wrapper_lru_page_class {
    ($(fn $name:ident;)*) => {$(
        kernel::macros::paste! {
            #[inline]
            pub(crate) fn [< trace_ $name >](pid: Pid, page_index: usize) {
                // SAFETY: Always safe to call.
                unsafe { [< rust_binder_ $name >](pid as c_int, page_index) }
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
pub(crate) fn trace_command(cmd: u32) {
    // SAFETY: Trivially safe to call with primitive u32.
    unsafe { rust_binder_command(cmd) }
}

#[inline]
pub(crate) fn trace_return(ret: u32) {
    // SAFETY: Trivially safe to call with primitive u32.
    unsafe { rust_binder_return(ret) }
}
