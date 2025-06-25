// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Intel Corporation
 */

#include <asm/trapnr.h>
#include <pkvm.h>
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
	u64 cur_vmcs_pa;

	if (!hvcpu || !vmx)
		return;

	if (hvcpu->pending_nmi) {
		pkvm_dbg("%s: CPU%d already has a pending NMI\n",
			__func__, cpu_id);
		return;
	}

	/* Save the current active VMCS physical address */
	cur_vmcs_pa = vmcs_store();

	/* load host vcpu vmcs for sure */
	vmcs_load(vmx->loaded_vmcs->vmcs);

	/*
	 * This NMI could happen either before executing
	 * the injection code or after.
	 * For the before case, should record a pending NMI.
	 * For the after case, if no NMI is injected in guest
	 * we also need to record a pending NMI. If NMI is
	 * injected already, it is not necessary to inject
	 * again but injecting it in the next round should also
	 * be fine. So simply record a pending NMI here.
	 */
	hvcpu->pending_nmi = true;

	pkvm_dbg("%s: CPU%d pending NMI\n", __func__, cpu_id);

	/* For case that when NMI happens the injection code is
	 * already executed, open the irq window. For the case
	 * happens before, opening irq window doesn't cause trouble.
	 */
	vmx_enable_irq_window(vmx);

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
