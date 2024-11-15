// SPDX-License-Identifier: GPL-2.0
#include <x86.h>

noinstr void kvm_spurious_fault(void)
{
#ifndef __PKVM_HYP__
	/* Fault while not rebooting.  We want the trace. */
	BUG_ON(!kvm_rebooting);
#endif
}
EXPORT_SYMBOL_GPL(kvm_spurious_fault);
