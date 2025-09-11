// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <asm/trapnr.h>
#include <pkvm.h>
#include <vmx/x86_ops.h>
#include <capabilities.h>
#include "cpu.h"
#include "pkvm_hyp.h"
#include "vmx.h"
#include "debug.h"
#include "irq.h"

static void handle_nmi(int cpu_id)
{
	struct pkvm_host_vcpu *hvcpu =
		pkvm_hyp->host_vm.host_vcpus[cpu_id];
	struct vcpu_vmx *vmx = &hvcpu->vmx;
	struct kvm_vcpu *vcpu = &vmx->vcpu;
	u64 cur_vmcs_pa;

	if (!hvcpu || !vmx)
		return;

	/* Save the current active VMCS physical address */
	cur_vmcs_pa = vmcs_store();

	/* load host vcpu vmcs for sure */
	vmcs_load(vmx->loaded_vmcs->vmcs);

	/*
	 * The NMI happens while the pkvm hypervisor is running, but it should
	 * be handled by the host. Record a pending NMI to be injected to the
	 * host.
	 */
	atomic_inc(&vcpu->arch.nmi_queued);
	kvm_make_request(KVM_REQ_NMI, vcpu);

	pkvm_dbg("%s: CPU%d pending NMI\n", __func__, cpu_id);

	/*
	 * Request host immediate exit in case the pending NMI has already been
	 * handled in this host vmexit handling cycle.
	 */
	request_host_immediate_exit(to_vmx(vcpu));

	/* Switch if the current one is not host vcpu vmcs */
	if (cur_vmcs_pa != __pkvm_pa(vmx->loaded_vmcs->vmcs))
		vmcs_load(__pkvm_va(cur_vmcs_pa));
}

void handle_exception(struct pt_regs *regs, int vector, bool has_error_code)
{
	int cpu = raw_smp_processor_id();

	if (vector == X86_TRAP_NMI)
		return handle_nmi(cpu);

	if (has_error_code)
		pkvm_err("pkvm: exception %d on CPU%d @ip %pS (0x%px), err code 0x%lx\n",
			 vector, cpu, (void *)regs->ip, (void *)regs->ip, regs->orig_ax);
	else
		pkvm_err("pkvm: exception %d on CPU%d @ip %pS (0x%px), no err code\n",
			 vector, cpu, (void *)regs->ip, (void *)regs->ip);

	asm volatile("hlt" : : : "memory");
}
