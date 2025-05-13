/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM gunyah
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_GUNYAH_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_GUNYAH_H
#include <trace/hooks/vendor_hooks.h>
struct gunyah_hypercall_vcpu_run_resp;
struct gunyah_vcpu;
struct gunyah_vm;

DECLARE_RESTRICTED_HOOK(android_rvh_gh_before_vcpu_run,
	     TP_PROTO(u16 vmid, u32 vcpu_id),
	     TP_ARGS(vmid, vcpu_id), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_gh_after_vcpu_run,
	     TP_PROTO(u16 vmid, u32 vcpu_id, int hcall_ret,
	     const struct gunyah_hypercall_vcpu_run_resp *resp),
	     TP_ARGS(vmid, vcpu_id, hcall_ret, resp), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_gh_vm_release,
	     TP_PROTO(u16 vmid, struct gunyah_vm *ghvm),
	     TP_ARGS(vmid, ghvm), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_gh_vcpu_release,
	     TP_PROTO(u16 vmid, struct gunyah_vcpu *vcpu),
	     TP_ARGS(vmid, vcpu), 1);

#endif /* _TRACE_HOOK_GUNYAH_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
