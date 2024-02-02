/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 MediaTek Inc.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mbox

#if !defined(_TRACE_MBOX_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MBOX_H

#include <linux/tracepoint.h>

TRACE_EVENT(mtk_mbox_isr_entry,

	TP_PROTO(const char *name, unsigned int irq_status),

	TP_ARGS(name, irq_status),

	TP_STRUCT__entry(
		__string(name, name)
		__field(unsigned int, irq_status)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->irq_status = irq_status;
	),

	TP_printk("[MBOX]dev=%s, irq_status=0x%x", __get_str(name), __entry->irq_status)
);

TRACE_EVENT(mtk_mbox_isr_exit,

	TP_PROTO(const char *name, unsigned int irq_status),

	TP_ARGS(name, irq_status),

	TP_STRUCT__entry(
		__string(name, name)
		__field(unsigned int, irq_status)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->irq_status = irq_status;
	),

	TP_printk("[MBOX]dev=%s, irq_status=0x%x", __get_str(name), __entry->irq_status)
);

TRACE_EVENT(mtk_mbox_polling,

	TP_PROTO(const char *name, unsigned int irq_status, unsigned int recv_pin_index),

	TP_ARGS(name, irq_status, recv_pin_index),

	TP_STRUCT__entry(
		__string(name, name)
		__field(unsigned int, irq_status)
		__field(unsigned int, recv_pin_index)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->irq_status = irq_status;
		__entry->recv_pin_index = recv_pin_index;
	),

	TP_printk("[MBOX]dev=%s polling irq_status=0x%x, recv_pin_index=%u before clear irq",
		__get_str(name), __entry->irq_status, __entry->recv_pin_index)
);

#endif /* _TRACE_MBOX_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
