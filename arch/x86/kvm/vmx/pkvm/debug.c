// SPDX-License-Identifier: GPL-2.0
#include <linux/printk.h>
#include <linux/dynamic_debug.h>
#include <linux/pid.h>
#include <asm/cpu_entry_area.h>
#include "debug.h"

#define PREFIX_SIZE 128

/* Return the path relative to source root */
static inline const char *trim_prefix(const char *path)
{
	int skip = strlen(__FILE__) - strlen("arch/x86/kvm/vmx/pkvm/debug.c");

	if (strncmp(path, __FILE__, skip))
		skip = 0; /* prefix mismatch, don't skip */

	return path + skip;
}

static int remaining(int wrote)
{
	if (PREFIX_SIZE - wrote > 0)
		return PREFIX_SIZE - wrote;
	return 0;
}

static char *__dynamic_emit_prefix(const struct _ddebug *desc, char *buf)
{
	int pos_after_tid;
	int pos = 0;

	if (desc->flags & _DPRINTK_FLAGS_INCL_TID) {
		if (in_interrupt())
			pos += snprintf(buf + pos, remaining(pos), "<intr> ");
		else
			pos += snprintf(buf + pos, remaining(pos), "[%d] ",
					task_pid_vnr(current));
	}
	pos_after_tid = pos;
	if (desc->flags & _DPRINTK_FLAGS_INCL_MODNAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				desc->modname);
	if (desc->flags & _DPRINTK_FLAGS_INCL_FUNCNAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				desc->function);
	if (desc->flags & _DPRINTK_FLAGS_INCL_SOURCENAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				trim_prefix(desc->filename));
	if (desc->flags & _DPRINTK_FLAGS_INCL_LINENO)
		pos += snprintf(buf + pos, remaining(pos), "%d:",
				desc->lineno);
	if (pos - pos_after_tid)
		pos += snprintf(buf + pos, remaining(pos), " ");
	if (pos >= PREFIX_SIZE)
		buf[PREFIX_SIZE - 1] = '\0';

	return buf;

}

static inline char *dynamic_emit_prefix(struct _ddebug *desc, char *buf)
{
	if (unlikely(desc->flags & _DPRINTK_FLAGS_INCL_ANY))
		return __dynamic_emit_prefix(desc, buf);
	return buf;
}

void pkvm_debug_sym(__dynamic_pr_debug)(struct _ddebug *descriptor, const char *fmt, ...)
{
	va_list args;
	struct va_format vaf;
	char buf[PREFIX_SIZE] = "";

	BUG_ON(!descriptor);
	BUG_ON(!fmt);

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_DEBUG "%s%pV", dynamic_emit_prefix(descriptor, buf), &vaf);

	va_end(args);
}

int pkvm_debug_sym(_printk)(const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);

	return r;
}

/* Is called from entry code, so must be noinstr */
noinstr struct cpu_entry_area *pkvm_debug_sym(get_cpu_entry_area)(int cpu)
{
	return get_cpu_entry_area(cpu);
}
