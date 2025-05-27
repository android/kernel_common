/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Google, Inc.
 */

#undef TRACE_SYSTEM
#undef TRACE_INCLUDE_FILE
#undef TRACE_INCLUDE_PATH
#define TRACE_SYSTEM rust_binder
#define TRACE_INCLUDE_FILE rust_binder_events
#define TRACE_INCLUDE_PATH ../drivers/android/binder

#if !defined(_RUST_BINDER_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _RUST_BINDER_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(rust_binder_ioctl,
	TP_PROTO(unsigned int cmd, unsigned long arg),
	TP_ARGS(cmd, arg),

	TP_STRUCT__entry(
		__field(unsigned int, cmd)
		__field(unsigned long, arg)
	),
	TP_fast_assign(
		__entry->cmd = cmd;
		__entry->arg = arg;
	),
	TP_printk("cmd=0x%x arg=0x%lx", __entry->cmd, __entry->arg)
);

DECLARE_EVENT_CLASS(rust_binder_function_return_class,
	TP_PROTO(int ret),
	TP_ARGS(ret),
	TP_STRUCT__entry(
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->ret = ret;
	),
	TP_printk("ret=%d", __entry->ret)
);

#define DEFINE_RBINDER_FUNCTION_RETURN_EVENT(name)	\
DEFINE_EVENT(rust_binder_function_return_class, name,	\
	TP_PROTO(int ret), \
	TP_ARGS(ret))

DEFINE_RBINDER_FUNCTION_RETURN_EVENT(rust_binder_ioctl_done);
DEFINE_RBINDER_FUNCTION_RETURN_EVENT(rust_binder_read_done);
DEFINE_RBINDER_FUNCTION_RETURN_EVENT(rust_binder_write_done);

TRACE_EVENT(rust_binder_set_priority,
	TP_PROTO(struct task_struct *thread, int desired_prio, int new_prio),
	TP_ARGS(thread, desired_prio, new_prio),

	TP_STRUCT__entry(
		__field(int, proc)
		__field(int, thread)
		__field(unsigned int, old_prio)
		__field(unsigned int, new_prio)
		__field(unsigned int, desired_prio)
	),
	TP_fast_assign(
		__entry->proc = thread->tgid;
		__entry->thread = thread->pid;
		__entry->old_prio = thread->normal_prio;
		__entry->new_prio = new_prio;
		__entry->desired_prio = desired_prio;
	),
	TP_printk("proc=%d thread=%d old=%d => new=%d desired=%d",
		  __entry->proc, __entry->thread, __entry->old_prio,
		  __entry->new_prio, __entry->desired_prio)
);

TRACE_EVENT(rust_binder_wait_for_work,
	TP_PROTO(bool proc_work, bool transaction_stack, bool thread_todo),
	TP_ARGS(proc_work, transaction_stack, thread_todo),

	TP_STRUCT__entry(
		__field(bool, proc_work)
		__field(bool, transaction_stack)
		__field(bool, thread_todo)
	),
	TP_fast_assign(
		__entry->proc_work = proc_work;
		__entry->transaction_stack = transaction_stack;
		__entry->thread_todo = thread_todo;
	),
	TP_printk("proc_work=%d transaction_stack=%d thread_todo=%d",
		  __entry->proc_work, __entry->transaction_stack,
		  __entry->thread_todo)
);

TRACE_EVENT(rust_binder_transaction,
	TP_PROTO(bool reply, rust_binder_transaction t),
	TP_ARGS(reply, t),
	TP_STRUCT__entry(
		__field(int, debug_id)
		__field(int, target_node)
		__field(int, from_proc)
		__field(int, to_proc)
		__field(int, reply)
		__field(unsigned int, code)
		__field(unsigned int, flags)
	),
	TP_fast_assign(
		rust_binder_thread from_thread = rust_binder_transaction_from_thread(t);
		rust_binder_process from = rust_binder_thread_proc(from_thread);
		rust_binder_process to = rust_binder_transaction_to_proc(t);
		rust_binder_node target_node = rust_binder_transaction_target_node(t);

		__entry->debug_id = rust_binder_transaction_debug_id(t);
		__entry->target_node = target_node ? rust_binder_node_debug_id(target_node) : 0;
		__entry->from_proc = rust_binder_process_task(from)->pid;
		__entry->to_proc = rust_binder_process_task(to)->pid;
		__entry->reply = reply;
		__entry->code = rust_binder_transaction_code(t);
		__entry->flags = rust_binder_transaction_flags(t);
	),
	TP_printk("transaction=%d target_node=%d dest_proc=%d from_proc=%d reply=%d flags=0x%x code=0x%x",
		__entry->debug_id, __entry->target_node, __entry->to_proc,
		__entry->from_proc, __entry->reply, __entry->flags,
		__entry->code)
);

TRACE_EVENT(rust_binder_transaction_thread_selected,
	TP_PROTO(rust_binder_transaction t, rust_binder_thread thread),
	TP_ARGS(t, thread),
	TP_STRUCT__entry(
		__field(int, debug_id)
		__field(int, to_thread)
	),
	TP_fast_assign(
		__entry->debug_id = rust_binder_transaction_debug_id(t);
		__entry->to_thread = rust_binder_thread_id(thread);
	),
	TP_printk("transaction=%d thread=%d", __entry->debug_id, __entry->to_thread)
);

TRACE_EVENT(rust_binder_transaction_received,
	TP_PROTO(rust_binder_transaction t),
	TP_ARGS(t),
	TP_STRUCT__entry(
		__field(int, debug_id)
	),
	TP_fast_assign(
		__entry->debug_id = rust_binder_transaction_debug_id(t);
	),
	TP_printk("transaction=%d", __entry->debug_id)
);

TRACE_EVENT(rust_binder_transaction_node_send,
	TP_PROTO(int t_debug_id, rust_binder_node node,
		const struct flat_binder_object *original,
		const struct flat_binder_object *translated),
	TP_ARGS(t_debug_id, node, original, translated),

	TP_STRUCT__entry(
		__field(int, debug_id)
		__field(int, node_debug_id)
		__field(binder_uintptr_t, node_ptr)
		__field(int, types)
		__field(int, original_handle)
		__field(int, translated_handle)
	),
	TP_fast_assign(
		int orig_is_handle = original->hdr.type == BINDER_TYPE_HANDLE || original->hdr.type == BINDER_TYPE_WEAK_HANDLE;
		int orig_is_strong = original->hdr.type == BINDER_TYPE_HANDLE || original->hdr.type == BINDER_TYPE_BINDER;
		int tran_is_handle = translated->hdr.type == BINDER_TYPE_HANDLE || translated->hdr.type == BINDER_TYPE_WEAK_HANDLE;
		int tran_is_strong = translated->hdr.type == BINDER_TYPE_HANDLE || translated->hdr.type == BINDER_TYPE_BINDER;

		__entry->debug_id = t_debug_id;
		__entry->node_debug_id = rust_binder_node_debug_id(node);
		__entry->node_ptr = rust_binder_node_debug_id(node);
		__entry->types =
			(orig_is_handle << 0) |
			(tran_is_handle << 1) |
			(orig_is_strong << 2) |
			(tran_is_strong << 3);
		__entry->original_handle = orig_is_handle ? original->handle : 0;
		__entry->translated_handle = tran_is_handle ? original->handle : 0;
	),
	TP_printk("transaction=%d node=%d ptr=0x%016llx: %s%s [%d] ==> %s%s [%d]",
		  __entry->debug_id, __entry->node_debug_id,
		  (u64)__entry->node_ptr,
		  (__entry->types & (1<<2)) ? "" : "weak ",
		  (__entry->types & (1<<0)) ? "handle" : "binder",
		  __entry->original_handle,
		  (__entry->types & (1<<3)) ? "" : "weak ",
		  (__entry->types & (1<<1)) ? "handle" : "binder",
		  __entry->translated_handle)
);

TRACE_EVENT(rust_binder_transaction_fd_send,
	TP_PROTO(int t_debug_id, int fd, size_t offset),
	TP_ARGS(t_debug_id, fd, offset),

	TP_STRUCT__entry(
		__field(int, debug_id)
		__field(int, fd)
		__field(size_t, offset)
	),
	TP_fast_assign(
		__entry->debug_id = t_debug_id;
		__entry->fd = fd;
		__entry->offset = offset;
	),
	TP_printk("transaction=%d src_fd=%d offset=%zu",
		  __entry->debug_id, __entry->fd, __entry->offset)
);

TRACE_EVENT(rust_binder_transaction_fd_recv,
	TP_PROTO(int t_debug_id, int fd, size_t offset),
	TP_ARGS(t_debug_id, fd, offset),

	TP_STRUCT__entry(
		__field(int, debug_id)
		__field(int, fd)
		__field(size_t, offset)
	),
	TP_fast_assign(
		__entry->debug_id = t_debug_id;
		__entry->fd = fd;
		__entry->offset = offset;
	),
	TP_printk("transaction=%d dest_fd=%d offset=%zu",
		  __entry->debug_id, __entry->fd, __entry->offset)
);

TRACE_EVENT(rust_binder_transaction_alloc_buf,
	TP_PROTO(int debug_id, const struct binder_transaction_data_sg *data),
	TP_ARGS(debug_id, data),

	TP_STRUCT__entry(
		__field(int, debug_id)
		__field(size_t, data_size)
		__field(size_t, offsets_size)
		__field(size_t, extra_buffers_size)
	),
	TP_fast_assign(
		__entry->debug_id = debug_id;
		__entry->data_size = data->transaction_data.data_size;
		__entry->offsets_size = data->transaction_data.offsets_size;
		__entry->extra_buffers_size = data->buffers_size;
	),
	TP_printk("transaction=%d data_size=%zd offsets_size=%zd extra_buffers_size=%zd",
		  __entry->debug_id, __entry->data_size, __entry->offsets_size,
		  __entry->extra_buffers_size)
);

DECLARE_EVENT_CLASS(rust_binder_buffer_release_class,
	TP_PROTO(int debug_id),
	TP_ARGS(debug_id),
	TP_STRUCT__entry(
		__field(int, debug_id)
	),
	TP_fast_assign(
		__entry->debug_id = debug_id;
	),
	TP_printk("transaction=%d", __entry->debug_id)
);

DEFINE_EVENT(rust_binder_buffer_release_class, rust_binder_transaction_buffer_release,
	TP_PROTO(int debug_id),
	TP_ARGS(debug_id));

DEFINE_EVENT(rust_binder_buffer_release_class, rust_binder_transaction_failed_buffer_release,
	TP_PROTO(int debug_id),
	TP_ARGS(debug_id));

DEFINE_EVENT(rust_binder_buffer_release_class, rust_binder_transaction_update_buffer_release,
	TP_PROTO(int debug_id),
	TP_ARGS(debug_id));

TRACE_EVENT(rust_binder_update_page_range,
	TP_PROTO(int pid, bool allocate, size_t start, size_t end),
	TP_ARGS(pid, allocate, start, end),
	TP_STRUCT__entry(
		__field(int, proc)
		__field(bool, allocate)
		__field(size_t, offset)
		__field(size_t, size)
	),
	TP_fast_assign(
		__entry->proc = pid;
		__entry->allocate = allocate;
		__entry->offset = start;
		__entry->size = end - start;
	),
	TP_printk("proc=%d allocate=%d offset=%zu size=%zu",
		  __entry->proc, __entry->allocate,
		  __entry->offset, __entry->size)
);

DECLARE_EVENT_CLASS(rust_binder_lru_page_class,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index),
	TP_STRUCT__entry(
		__field(int, proc)
		__field(size_t, page_index)
	),
	TP_fast_assign(
		__entry->proc = pid;
		__entry->page_index = page_index;
	),
	TP_printk("proc=%d page_index=%zu",
		  __entry->proc, __entry->page_index)
);

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_alloc_lru_start,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_alloc_lru_end,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_free_lru_start,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_free_lru_end,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_alloc_page_start,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_alloc_page_end,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_unmap_user_start,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_unmap_user_end,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_unmap_kernel_start,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

DEFINE_EVENT(rust_binder_lru_page_class, rust_binder_unmap_kernel_end,
	TP_PROTO(int pid, size_t page_index),
	TP_ARGS(pid, page_index));

TRACE_EVENT(rust_binder_command,
	TP_PROTO(uint32_t cmd),
	TP_ARGS(cmd),
	TP_STRUCT__entry(
		__field(uint32_t, cmd)
	),
	TP_fast_assign(
		__entry->cmd = cmd;
	),
	TP_printk("cmd=0x%x %s",
		  __entry->cmd,
		  _IOC_NR(__entry->cmd) < ARRAY_SIZE(binder_command_strings) ?
			  binder_command_strings[_IOC_NR(__entry->cmd)] :
			  "unknown")
);

TRACE_EVENT(rust_binder_return,
    TP_PROTO(uint32_t ret),
    TP_ARGS(ret),
    TP_STRUCT__entry(
        __field(uint32_t, ret)
    ),
    TP_fast_assign(
        __entry->ret = ret;
    ),
    TP_printk("ret=0x%x %s",
          __entry->ret,
          _IOC_NR(__entry->ret) < ARRAY_SIZE(binder_return_strings) ?
              binder_return_strings[_IOC_NR(__entry->ret)] :
              "unknown")
);

#endif /* _RUST_BINDER_TRACE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
