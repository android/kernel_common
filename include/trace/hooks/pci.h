/* SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM pci
#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_PCI_VH_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_PCI_VH_H
#include <trace/hooks/vendor_hooks.h>

struct pci_dev;
typedef int __bitwise pci_power_t;

DECLARE_HOOK(android_vh_platform_pci_power_manageable,
		TP_PROTO(struct pci_dev *dev, bool *manageable),
		TP_ARGS(dev, manageable));
DECLARE_HOOK(android_vh_platform_pci_set_power_state,
		TP_PROTO(struct pci_dev *dev, pci_power_t t, int *ret),
		TP_ARGS(dev, t, ret));
DECLARE_HOOK(android_vh_platform_pci_get_power_state,
		TP_PROTO(struct pci_dev *dev, pci_power_t *state),
		TP_ARGS(dev, state));
DECLARE_HOOK(android_vh_platform_pci_choose_state,
		TP_PROTO(struct pci_dev *dev, pci_power_t *state),
		TP_ARGS(dev, state));

#endif /* _TRACE_HOOK_PCI_VH_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
