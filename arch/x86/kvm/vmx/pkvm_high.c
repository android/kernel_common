// SPDX-License-Identifier: GPL-2.0
#include <linux/moduleparam.h>
#include <asm/perf_event.h>
#include <asm/kvm_pkvm.h>
#include <asm/fred.h>
#include <asm/pkvm.h>
#include "pkvm.h"
#include "pkvm_constants.h"

#include "x86_ops.h"
#include "vmx.h"
#include "nested.h"
#include "pmu.h"
#include "posted_intr.h"
#include <trace/events/ipi.h>
#include "trace.h"

static DEFINE_PER_CPU(union pkvm_pv_param, *pv_param);

#define get_this_pv_param(f)		(&per_cpu(pv_param, get_cpu())->f)
#define put_this_pv_param(ptr)		\
({					\
	memset(ptr, 0, sizeof(*ptr));	\
	ptr = NULL;			\
	put_cpu();			\
})

static void pkvm_mc_free_fn(void *addr, void *unused)
{
	free_page((unsigned long)addr);
}

static void *kvm_host_va(phys_addr_t phys)
{
	return __va(phys);
}

static void free_pkvm_memcache(struct pkvm_memcache *mc)
{
	__free_pkvm_memcache(mc, pkvm_mc_free_fn,
			     kvm_host_va, NULL);
}

static inline bool cpu_need_virtualize_apic_accesses(struct kvm_vcpu *vcpu)
{
	return flexpriority_enabled && lapic_in_kernel(vcpu);
}

static void free_pml_buffer(struct vcpu_vmx *vmx)
{
	if (vmx->pml_pg) {
		free_page((unsigned long)vmx->pml_pg);
		vmx->pml_pg = NULL;
	}
}

static void free_ve_info(struct vcpu_vmx *vmx)
{
	if (vmx->ve_info) {
		free_page((unsigned long)vmx->ve_info);
		vmx->ve_info = NULL;
	}
}

static void pkvm_free_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	if (!loaded_vmcs->vmcs)
		return;
	free_vmcs(loaded_vmcs->vmcs);
	loaded_vmcs->vmcs = NULL;
	if (loaded_vmcs->msr_bitmap)
		free_page((unsigned long)loaded_vmcs->msr_bitmap);
	WARN_ON(loaded_vmcs->shadow_vmcs != NULL);
}

static int pkvm_alloc_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	loaded_vmcs->vmcs = alloc_vmcs(false);
	if (!loaded_vmcs->vmcs)
		return -ENOMEM;

	loaded_vmcs->shadow_vmcs = NULL;
	loaded_vmcs->hv_timer_soft_disabled = false;
	loaded_vmcs->cpu = -1;

	if (cpu_has_vmx_msr_bitmap()) {
		loaded_vmcs->msr_bitmap = (unsigned long *)
				__get_free_page(GFP_KERNEL_ACCOUNT);
		if (!loaded_vmcs->msr_bitmap)
			goto out_vmcs;
	}

	memset(&loaded_vmcs->host_state, 0, sizeof(struct vmcs_host_state));
	memset(&loaded_vmcs->controls_shadow, 0,
		sizeof(struct vmcs_controls_shadow));

	return 0;

out_vmcs:
	pkvm_free_loaded_vmcs(loaded_vmcs);
	return -ENOMEM;
}

static void __pkvm_vcpu_unload(void *arg)
{
	struct kvm_vcpu *vcpu = arg;
	struct vcpu_vmx *vmx;

	kvm_call_pkvm(vcpu_put, vcpu);

	vmx = to_vmx(vcpu);
	vmx->loaded_vmcs->cpu = -1;
}

static void pkvm_vcpu_unload(struct kvm_vcpu *vcpu)
{
	int cpu = to_vmx(vcpu)->loaded_vmcs->cpu;

	if (cpu != -1)
		smp_call_function_single(cpu, __pkvm_vcpu_unload, vcpu, 1);
}

static bool pkvm_segment_cache_test(struct vcpu_vmx *vmx, int seg, int field)
{
	u32 mask = 1 << (seg * SEG_FIELD_NR + field);

	if (!kvm_register_is_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS)) {
		kvm_register_mark_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS);
		vmx->segment_cache.bitmask = 0;
	}

	return vmx->segment_cache.bitmask & mask;
}

static void pkvm_segment_cache_set(struct vcpu_vmx *vmx, int seg, int field)
{
	u32 mask = 1 << (seg * SEG_FIELD_NR + field);

	if (!kvm_register_is_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS)) {
		kvm_register_mark_available(&vmx->vcpu, VCPU_EXREG_SEGMENTS);
		vmx->segment_cache.bitmask = 0;
	}

	vmx->segment_cache.bitmask |= mask;
}

static void pkvm_cache_segment(struct vcpu_vmx *vmx, struct kvm_segment *var, int seg)
{
	struct kvm_save_segment *save = &vmx->segment_cache.seg[seg];

	save->selector = var->selector;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_SEL);

	save->base = var->base;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_BASE);

	save->limit = var->limit;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_LIMIT);

	save->ar = (var->unusable << 16) |
		      (var->g << 15)	 |
		      (var->db << 14)	 |
		      (var->l << 13)	 |
		      (var->avl << 12)	 |
		      (var->present << 7)	 |
		      (var->dpl << 5)	 |
		      (var->s << 4)	 |
		      var->type;
	pkvm_segment_cache_set(vmx, seg, SEG_FIELD_AR);
}

static bool pkvm_hyp_emulated_msr(u32 msr)
{
	switch (msr) {
#ifdef CONFIG_X86_64
	case MSR_FS_BASE:
	case MSR_GS_BASE:
	case MSR_KERNEL_GS_BASE:
#endif
	case MSR_IA32_SYSENTER_CS:
	case MSR_IA32_SYSENTER_EIP:
	case MSR_IA32_SYSENTER_ESP:
	case MSR_IA32_DEBUGCTLMSR:
	case MSR_IA32_BNDCFGS:
	case MSR_IA32_UMWAIT_CONTROL:
	case MSR_IA32_SPEC_CTRL:
	case MSR_IA32_TSX_CTRL:
	case MSR_IA32_MCG_EXT_CTL:
	case MSR_IA32_FEAT_CTL:
	case MSR_IA32_SGXLEPUBKEYHASH0 ... MSR_IA32_SGXLEPUBKEYHASH3:
	case MSR_IA32_XSS:
		return true;
	default:
		break;
	}

	return false;
}

static int pkvm_check_processor_compat(void)
{
	return kvm_call_pkvm(check_processor_compatibility);
}

static int pkvm_enable_virtualization_cpu(void)
{
	size_t size = sizeof(union pkvm_pv_param);
	void *page;
	int r;

	page = alloc_pages_exact(size, GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	/* TODO: share union pkvm_pv_param with pkvm */
	r = kvm_call_pkvm(enable_virtualization_cpu, __pa(page));
	if (r) {
		free_pages_exact(page, size);
		return r;
	}

	this_cpu_write(pv_param, page);
	intel_pt_handle_vmx(1);
	return 0;
}

static void pkvm_disable_virtualization_cpu(void)
{
	size_t size = sizeof(union pkvm_pv_param);
	void *page = this_cpu_read(pv_param);

	intel_pt_handle_vmx(0);
	kvm_call_pkvm(disable_virtualization_cpu);
	/* TODO: unshare union pkvm_pv_param with pkvm */
	free_pages_exact(page, size);
	this_cpu_write(pv_param, NULL);
}

static void pkvm_emergency_disable_virtualization_cpu(void) { /* TODO */ }

static bool pkvm_has_emulated_msr(struct kvm *kvm, u32 index)
{
	switch (index) {
	case MSR_IA32_SMBASE:
		/* The guest SMM mode is not supported. */
	case KVM_FIRST_EMULATED_VMX_MSR ... KVM_LAST_EMULATED_VMX_MSR:
		/* The guest VMX feature is not supported */
	case MSR_AMD64_VIRT_SPEC_CTRL:
	case MSR_AMD64_TSC_RATIO:
		/* This is AMD only.  */
		return false;
	default:
		/*
		 * The MSR not emulated by the pkvm hypervisor can be emualted
		 * by the host.
		 */
		return !pkvm_hyp_emulated_msr(index);
	}
}

static int pkvm_vm_init(struct kvm *kvm)
{
	struct kvm_protected_vm *pkvm = &kvm->arch.pkvm;
	size_t pkvm_vm_sz;
	void *pkvm_vm;
	int ret;

	ret = vmx_vm_init(kvm);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&pkvm->pinned_pages);
	pkvm->pvmfw_load_addr = PVMFW_INVALID_LOAD_ADDR;

	pkvm_vm_sz = PAGE_ALIGN(PKVM_SHADOW_VM_SIZE);
	pkvm_vm = alloc_pages_exact(pkvm_vm_sz, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vm)
		return -ENOMEM;

	/* TODO: share struct kvm_vmx with pkvm */

	ret = kvm_call_pkvm(vm_init, kvm, __pa(pkvm_vm));
	if (ret < 0)
		goto free_page;

	pkvm->pkvm_vm_handle = ret;

	return 0;

free_page:
	free_pages_exact(pkvm_vm, pkvm_vm_sz);
	return ret;
}

static void pkvm_vm_destroy(struct kvm *kvm)
{
	struct kvm_protected_vm *pkvm = &kvm->arch.pkvm;
	struct kvm_pinned_page *ppage, *n;
	struct kvm_vcpu *vcpu;
	unsigned long i;
	int ret;

	/*
	 * Make sure each vcpu is unloaded in the pkvm hypervisor before destroy
	 * VM.
	 */
	kvm_for_each_vcpu(i, vcpu, kvm)
		pkvm_vcpu_unload(vcpu);

	ret = kvm_call_pkvm(vm_destroy, pkvm->pkvm_vm_handle);
	if (ret)
		return;

	/* TODO: unshare struct kvm_vmx with pkvm */

	free_pkvm_memcache(&pkvm->teardown_mc);

	list_for_each_entry_safe(ppage, n, &pkvm->pinned_pages, list) {
		list_del(&ppage->list);
		put_page(ppage->page);
		kfree(ppage);
	}

	vmx_vm_destroy(kvm);
}

static int pkvm_vcpu_create(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx;
	size_t pkvm_vcpu_sz;
	struct page *page;
	void *pkvm_vcpu;
	int ret;

	BUILD_BUG_ON(offsetof(struct vcpu_vmx, vcpu) != 0);
	vmx = to_vmx(vcpu);

	INIT_LIST_HEAD(&vmx->pi_wakeup_list);

	/*
	 * If PML is turned on, failure on enabling PML just results in failure
	 * of creating the vcpu, therefore we can simplify PML logic (by
	 * avoiding dealing with cases, such as enabling PML partially on vcpus
	 * for the guest), etc.
	 */
	if (enable_pml) {
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page)
			return -ENOMEM;
		vmx->pml_pg = page_to_virt(page);
	}

	ret = pkvm_alloc_loaded_vmcs(&vmx->vmcs01);
	if (ret < 0)
		goto free_pml;

	vmx->loaded_vmcs = &vmx->vmcs01;
	vmx->loaded_vmcs->cpu = -1;

	if (cpu_need_virtualize_apic_accesses(vcpu)) {
		ret = kvm_alloc_apic_access_page(vcpu->kvm);
		if (ret)
			goto free_vmcs;
	}

	ret = -ENOMEM;

	if (vmcs_config.cpu_based_2nd_exec_ctrl & SECONDARY_EXEC_EPT_VIOLATION_VE) {
		BUILD_BUG_ON(sizeof(*vmx->ve_info) > PAGE_SIZE);

		/* ve_info must be page aligned. */
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page)
			goto free_vmcs;

		vmx->ve_info = page_to_virt(page);
	}

	pkvm_vcpu_sz = PAGE_ALIGN(PKVM_SHADOW_VCPU_STATE_SIZE);
	pkvm_vcpu = alloc_pages_exact(pkvm_vcpu_sz, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vcpu)
		goto free_ve;

	/* TODO: share struct vcpu_vmx with pkvm */

	ret = kvm_call_pkvm(vcpu_create, vcpu, __pa(pkvm_vcpu));
	if (ret < 0)
		goto free_pages;

	vcpu->arch.pkvm_vcpu_handle = ret;

	return 0;

free_pages:
	free_pages_exact(pkvm_vcpu, pkvm_vcpu_sz);
free_ve:
	free_ve_info(vmx);
free_vmcs:
	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
free_pml:
	free_pml_buffer(vmx);
	return ret;
}

static void pkvm_vcpu_free(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	/* TODO: unshare struct vcpu_vmx with pkvm */

	if (enable_pml)
		free_pml_buffer(vmx);
	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
	free_ve_info(vmx);
}

static void pkvm_vcpu_reset(struct kvm_vcpu *vcpu, bool init_event)
{
	if (!vcpu->arch.guest_state_protected)
		kvm_call_pkvm(vcpu_reset, vcpu, init_event);

	/*
	 * The CR0/CR4 guest-owned/rsvd bits are controlled by the pkvm
	 * hypervisor. The host VMM can assume all the bits in CR0/CR4 are owned
	 * by the guest.
	 */
	vcpu->arch.cr0_guest_owned_bits = ~0;
	vcpu->arch.cr4_guest_owned_bits = ~0;
	vcpu->arch.cr4_guest_rsvd_bits = 0;

	kvm_set_cr8(vcpu, 0);
	kvm_make_request(KVM_REQ_APIC_PAGE_RELOAD, vcpu);
}

static void pkvm_prepare_switch_to_guest(struct kvm_vcpu *vcpu) {}

static void pkvm_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	bool already_loaded;

	already_loaded = vmx->loaded_vmcs->cpu == cpu;
	if (!already_loaded)
		pkvm_vcpu_unload(vcpu);

	kvm_call_pkvm(vcpu_load, vcpu, cpu);

	if (!already_loaded)
		vmx->loaded_vmcs->cpu = cpu;

	vmx_vcpu_pi_load(vcpu, cpu);
}

static void pkvm_vcpu_put(struct kvm_vcpu *vcpu)
{
	vmx_vcpu_pi_put(vcpu);
}

static void pkvm_update_exception_bitmap(struct kvm_vcpu *vcpu)
{
	/* TODO: Support for npVM */
}

static int pkvm_get_feature_msr(u32 msr, u64 *data)
{
	switch (msr) {
	case KVM_FIRST_EMULATED_VMX_MSR ... KVM_LAST_EMULATED_VMX_MSR:
		return 1;
	default:
		return KVM_MSR_RET_UNSUPPORTED;
	}
}

static int pkvm_get_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	/* Use PV interface to get the MSR emulated by the pkvm hypervisor */
	if (pkvm_hyp_emulated_msr(msr_info->index)) {
		if (!vcpu->arch.guest_state_protected) {
			struct msr_data *msr = get_this_pv_param(msr);
			int ret;

			*msr = *msr_info;
			ret = kvm_call_pkvm(get_msr, vcpu, msr);
			msr_info->data = msr->data;
			put_this_pv_param(msr);

			return ret;
		} else {
			return -EINVAL;
		}
	}

	/* Otherwise handle by the host VMM itself */
	return kvm_get_msr_common(vcpu, msr_info);
}

static int pkvm_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	/* Use PV interface to set the MSR emulated by the pkvm hypervisor */
	if (pkvm_hyp_emulated_msr(msr_info->index)) {
		if (!vcpu->arch.guest_state_protected) {
			struct msr_data *msr = get_this_pv_param(msr);
			int ret;

			*msr = *msr_info;
			ret = kvm_call_pkvm(set_msr, vcpu, msr);
			put_this_pv_param(msr);

			return ret;
		} else {
			return -EINVAL;
		}
	}

	/* Otherwise handle by the host VMM itself */
	return kvm_set_msr_common(vcpu, msr_info);
}

static u64 pkvm_get_segment_base(struct kvm_vcpu *vcpu, int seg)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	ulong *p;

	if (vcpu->arch.guest_state_protected)
		return 0;

	p = &vmx->segment_cache.seg[seg].base;

	if (!pkvm_segment_cache_test(vmx, seg, SEG_FIELD_BASE)) {
		*p = kvm_call_pkvm(get_segment_base, vcpu, seg);
		pkvm_segment_cache_set(vmx, seg, SEG_FIELD_BASE);
	}

	return *p;
}

static void pkvm_get_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	struct kvm_save_segment *segment;
	u32 ar;

	if (vcpu->arch.guest_state_protected) {
		memset(var, 0, sizeof(*var));
		return;
	}

	if (!pkvm_segment_cache_test(vmx, seg, SEG_FIELD_SEL) ||
	    !pkvm_segment_cache_test(vmx, seg, SEG_FIELD_BASE) ||
	    !pkvm_segment_cache_test(vmx, seg, SEG_FIELD_LIMIT) ||
	    !pkvm_segment_cache_test(vmx, seg, SEG_FIELD_AR)) {
		struct kvm_segment *pkvm_var = get_this_pv_param(seg);

		kvm_call_pkvm(get_segment, vcpu, pkvm_var, seg);

		pkvm_cache_segment(vmx, pkvm_var, seg);

		put_this_pv_param(pkvm_var);
	}

	segment = &vmx->segment_cache.seg[seg];
	var->selector = segment->selector;
	var->base = segment->base;
	var->limit = segment->limit;
	ar = segment->ar;
	var->unusable = (ar >> 16) & 1;
	var->type = ar & 15;
	var->s = (ar >> 4) & 1;
	var->dpl = (ar >> 5) & 3;
	/*
	 * Some userspaces do not preserve unusable property. Since usable
	 * segment has to be present according to VMX spec we can use present
	 * property to amend userspace bug by making unusable segment always
	 * nonpresent. vmx_segment_access_rights() already marks nonpresent
	 * segment as unusable.
	 */
	var->present = !var->unusable;
	var->avl = (ar >> 12) & 1;
	var->l = (ar >> 13) & 1;
	var->db = (ar >> 14) & 1;
	var->g = (ar >> 15) & 1;
}

static void pkvm_set_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg)
{
	struct kvm_segment *pkvm_var;

	if (vcpu->arch.guest_state_protected)
		return;

	pkvm_var = get_this_pv_param(seg);
	*pkvm_var = *var;
	kvm_call_pkvm(set_segment, vcpu, pkvm_var, seg);
	put_this_pv_param(pkvm_var);
}

static bool pkvm_is_valid_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
	return true;
}

static void pkvm_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
	if (!vcpu->arch.guest_state_protected)
		kvm_call_pkvm(set_cr0, vcpu, cr0);

	vcpu->arch.cr0 = cr0;
}

static bool pkvm_is_valid_cr4(struct kvm_vcpu *vcpu, unsigned long cr4)
{
	/* No VMX emulation in the pkvm hypervisor */
	if (cr4 & X86_CR4_VMXE)
		return false;

	return true;
}

static void pkvm_set_cr4(struct kvm_vcpu *vcpu, unsigned long cr4)
{
	if (!vcpu->arch.guest_state_protected)
		kvm_call_pkvm(set_cr4, vcpu, cr4);

	vcpu->arch.cr4 = cr4;
}

static int pkvm_set_efer(struct kvm_vcpu *vcpu, u64 efer)
{
	int ret = -EINVAL;

	if (!vcpu->arch.guest_state_protected)
		ret = kvm_call_pkvm(set_efer, vcpu, efer);

	vcpu->arch.efer = efer;
	return ret;
}

static void pkvm_access_idt_gdt(struct kvm_vcpu *vcpu, struct desc_ptr *dt,
				bool set, bool idt)
{
	struct desc_ptr *desc;

	if (vcpu->arch.guest_state_protected) {
		if (!set)
			memset(dt, 0, sizeof(*dt));
		return;
	}

	desc = get_this_pv_param(desc);

	if (set) {
		desc->size = dt->size;
		desc->address = dt->address;
		if (idt)
			kvm_call_pkvm(set_idt, vcpu, desc);
		else
			kvm_call_pkvm(set_gdt, vcpu, desc);
	} else {
		if (idt)
			kvm_call_pkvm(get_idt, vcpu, desc);
		else
			kvm_call_pkvm(get_gdt, vcpu, desc);
		dt->size = desc->size;
		dt->address = desc->address;
	}

	put_this_pv_param(desc);
}

static void pkvm_get_idt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, false, true);
}

static void pkvm_set_idt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, true, true);
}

static void pkvm_get_gdt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, false, false);
}

static void pkvm_set_gdt(struct kvm_vcpu *vcpu, struct desc_ptr *dt)
{
	pkvm_access_idt_gdt(vcpu, dt, true, false);
}

static void pkvm_set_dr7(struct kvm_vcpu *vcpu, unsigned long val)
{
	if (vcpu->arch.guest_state_protected)
		return;

	kvm_call_pkvm(set_dr7, vcpu, val);
}

static unsigned long pkvm_get_rflags(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!kvm_register_is_available(vcpu, VCPU_EXREG_RFLAGS)) {
		kvm_register_mark_available(vcpu, VCPU_EXREG_RFLAGS);
		if (vcpu->arch.guest_state_protected)
			vmx->rflags = 0;
		else
			vmx->rflags = kvm_call_pkvm(get_rflags, vcpu);
	}

	return vmx->rflags;
}

static void pkvm_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags)
{
	kvm_register_mark_available(vcpu, VCPU_EXREG_RFLAGS);
	to_vmx(vcpu)->rflags = rflags;
	if (vcpu->arch.guest_state_protected)
		return;
	kvm_call_pkvm(set_rflags, vcpu, rflags);
}

static bool pkvm_get_if_flag(struct kvm_vcpu *vcpu)
{
	return pkvm_get_rflags(vcpu) & X86_EFLAGS_IF;
}

static void pkvm_flush_tlb_all(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(flush_tlb_all, vcpu);
}

static void pkvm_flush_tlb_current(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(flush_tlb_current, vcpu);
}

static void pkvm_flush_tlb_gva(struct kvm_vcpu *vcpu, gva_t addr)
{
	kvm_call_pkvm(flush_tlb_gva, vcpu, addr);
}

static void pkvm_flush_tlb_guest(struct kvm_vcpu *vcpu)
{
	kvm_call_pkvm(flush_tlb_guest, vcpu);
}

void vmx_do_nmi_irqoff(void);

static fastpath_t pkvm_vcpu_run(struct kvm_vcpu *vcpu, bool force_immediate_exit)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long reqs_to_host;
	fastpath_t exit_fastpath;

	/* Record the guest's net vcpu time for enforced NMI injections. */
	if (unlikely(!enable_vnmi &&
		     vmx->loaded_vmcs->soft_vnmi_blocked))
		vmx->loaded_vmcs->entry_time = ktime_get();

	trace_kvm_entry(vcpu, force_immediate_exit);

	kvm_wait_lapic_expire(vcpu);

	guest_state_enter_irqoff();

	/*
	 * L1D Flush includes CPU buffer clear to mitigate MDS, but VERW
	 * mitigation for MDS is done late in VMentry and is still
	 * executed in spite of L1D Flush. This is because an extra VERW
	 * should not matter much after the big hammer L1D Flush.
	 */
	if (static_branch_unlikely(&vmx_l1d_should_flush))
		vmx_l1d_flush(vcpu);
	else if (static_branch_unlikely(&mmio_stale_data_clear) &&
		 kvm_arch_has_assigned_device(vcpu->kvm))
		mds_clear_cpu_buffers();

	vcpu->arch.nmi_injected = false;
	kvm_clear_interrupt_queue(vcpu);

	vmx->exit_reason.full = 0xdead;

	vcpu->arch.regs_avail &= ~VMX_REGS_LAZY_LOAD_SET;
	reqs_to_host = kvm_call_pkvm(vcpu_run, vcpu, force_immediate_exit);
	vcpu->arch.regs_dirty = 0;

	if (unlikely(vmx->exit_reason.full == 0xdead)) {
		vmx->fail = 1;
		return EXIT_FASTPATH_NONE;
	}

	if ((u16)vmx->exit_reason.basic == EXIT_REASON_EXCEPTION_NMI &&
	    is_nmi(vmx_get_intr_info(vcpu))) {
		kvm_before_interrupt(vcpu, KVM_HANDLING_NMI);
		if (cpu_feature_enabled(X86_FEATURE_FRED))
			fred_entry_from_kvm(EVENT_TYPE_NMI, NMI_VECTOR);
		else
			vmx_do_nmi_irqoff();
		kvm_after_interrupt(vcpu);
	}

	guest_state_exit_irqoff();

	if (unlikely((u16)vmx->exit_reason.basic == EXIT_REASON_MCE_DURING_VMENTRY))
		kvm_machine_check();

	trace_kvm_exit(vcpu, KVM_ISA_VMX);

	if (unlikely(vmx->exit_reason.failed_vmentry))
		return EXIT_FASTPATH_NONE;

	if (unlikely(!enable_vnmi &&
		     vmx->loaded_vmcs->soft_vnmi_blocked))
		vmx->loaded_vmcs->vnmi_blocked_time +=
			ktime_to_ns(ktime_sub(ktime_get(),
					      vmx->loaded_vmcs->entry_time));

	exit_fastpath = EXIT_FASTPATH_EXIT_HANDLED;
	if (reqs_to_host) {
		if (test_and_clear_bit(HOST_HANDLE_EXIT, &reqs_to_host))
			exit_fastpath = EXIT_FASTPATH_NONE;
	}

	if (exit_fastpath == EXIT_FASTPATH_EXIT_HANDLED)
		return exit_fastpath;

	return vmx_exit_handlers_fastpath(vcpu, force_immediate_exit);
}

static void pkvm_update_emulated_instruction(struct kvm_vcpu *vcpu) {}

static void pkvm_set_interrupt_shadow(struct kvm_vcpu *vcpu, int mask)
{
	if (vcpu->arch.guest_state_protected)
		return;

	kvm_call_pkvm(set_interrupt_shadow, vcpu, mask);
}

static u32 pkvm_get_interrupt_shadow(struct kvm_vcpu *vcpu)
{
	if (vcpu->arch.guest_state_protected)
		return 0;

	return kvm_call_pkvm(get_interrupt_shadow, vcpu);
}

static void pkvm_inject_irq(struct kvm_vcpu *vcpu, bool reinjected)
{
	trace_kvm_inj_virq(vcpu->arch.interrupt.nr,
			   vcpu->arch.interrupt.soft, reinjected);

	++vcpu->stat.irq_injections;

	kvm_call_pkvm(inject_irq, vcpu);
}

static void pkvm_inject_nmi(struct kvm_vcpu *vcpu)
{
	++vcpu->stat.nmi_injections;

	kvm_call_pkvm(inject_nmi, vcpu);
}

static int pkvm_interrupt_allowed(struct kvm_vcpu *vcpu, bool for_injection)
{
	return kvm_call_pkvm(interrupt_allowed, vcpu, for_injection);
}

static int pkvm_nmi_allowed(struct kvm_vcpu *vcpu, bool for_injection)
{
	return kvm_call_pkvm(nmi_allowed, vcpu, for_injection);
}

static void pkvm_vcpu_after_set_cpuid(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *e2 = vcpu->arch.cpuid_entries;
	int nent = vcpu->arch.cpuid_nent;
	unsigned long unused_pa;
	void *entries;
	size_t size;

	if (vcpu->arch.guest_state_protected || !e2 || !nent)
		return;

	size = sizeof(struct kvm_cpuid_entry2) * nent;
	entries = alloc_pages_exact(size, GFP_KERNEL_ACCOUNT);
	if (!entries) {
		kvm_err("Failed to allocate cpuid pages for pkvm vcpu\n");
		return;
	}

	memcpy(entries, (void *)e2, size);

	unused_pa = kvm_call_pkvm(vcpu_after_set_cpuid, vcpu, __pa(entries));
	if (VALID_PAGE(unused_pa)) {
		entries = __va(unused_pa);
		free_pages_exact(entries, size);
	}
}

static u64 pkvm_get_l2_tsc_offset(struct kvm_vcpu *vcpu)
{
	return 0;
}

static u64 pkvm_get_l2_tsc_multiplier(struct kvm_vcpu *vcpu)
{
	return kvm_caps.default_tsc_scaling_ratio;
}

static int pkvm_check_intercept(struct kvm_vcpu *vcpu,
				struct x86_instruction_info *info,
				enum x86_intercept_stage stage,
				struct x86_exception *exception)
{
	return X86EMUL_UNHANDLEABLE;
}

#ifdef CONFIG_KVM_SMM
static int pkvm_smi_allowed(struct kvm_vcpu *vcpu, bool for_injection)
{
	return false;
}

static int pkvm_enter_smm(struct kvm_vcpu *vcpu, union kvm_smram *smram)
{
	return -EINVAL;
}

static int pkvm_leave_smm(struct kvm_vcpu *vcpu, const union kvm_smram *smram)
{
	return -EINVAL;
}

static void pkvm_enable_smi_window(struct kvm_vcpu *vcpu) {}
#endif

static bool pkvm_apic_init_signal_blocked(struct kvm_vcpu *vcpu)
{
	return false;
}

static void pkvm_migrate_timers(struct kvm_vcpu *vcpu) {}

static int pkvm_complete_emulated_msr(struct kvm_vcpu *vcpu, int err)
{
	if (err)
		return kvm_call_pkvm(complete_emulated_msr, vcpu, err);

	return pkvm_is_protected_vcpu(vcpu) ? 1 : kvm_skip_emulated_instruction(vcpu);
}

#define VMX_REQUIRED_APICV_INHIBITS				\
	(BIT(APICV_INHIBIT_REASON_DISABLED) |			\
	 BIT(APICV_INHIBIT_REASON_ABSENT) |			\
	 BIT(APICV_INHIBIT_REASON_HYPERV) |			\
	 BIT(APICV_INHIBIT_REASON_BLOCKIRQ) |			\
	 BIT(APICV_INHIBIT_REASON_PHYSICAL_ID_ALIASED) |	\
	 BIT(APICV_INHIBIT_REASON_APIC_ID_MODIFIED) |		\
	 BIT(APICV_INHIBIT_REASON_APIC_BASE_MODIFIED))

static void pkvm_leave_nested(struct kvm_vcpu *vcpu) {}
static bool pkvm_nested_is_exception_vmexit(struct kvm_vcpu *vcpu, u8 vector,
					    u32 error_code)
{
	return false;
}
static int pkvm_check_nested_events(struct kvm_vcpu *vcpu) { return 0; }
static bool pkvm_has_nested_events(struct kvm_vcpu *vcpu, bool for_injection) { return false; }
static void pkvm_nested_triple_fault(struct kvm_vcpu *vcpu) {}
static int pkvm_get_nested_state(struct kvm_vcpu *vcpu,
				 struct kvm_nested_state __user *user_kvm_nested_state,
				 u32 user_data_size)
{
	return -EINVAL;
}
static int pkvm_set_nested_state(struct kvm_vcpu *vcpu,
				 struct kvm_nested_state __user *user_kvm_nested_state,
				 struct kvm_nested_state *kvm_state)
{
	return -EINVAL;
}
static bool pkvm_get_nested_state_pages(struct kvm_vcpu *vcpu) { return true; }
static int pkvm_nested_write_pml_buffer(struct kvm_vcpu *vcpu, gpa_t gpa) { return 0; }

static struct kvm_x86_nested_ops pkvm_nested_ops = {
	.leave_nested = pkvm_leave_nested,
	.is_exception_vmexit = pkvm_nested_is_exception_vmexit,
	.check_events = pkvm_check_nested_events,
	.has_events = pkvm_has_nested_events,
	.triple_fault = pkvm_nested_triple_fault,
	.get_state = pkvm_get_nested_state,
	.set_state = pkvm_set_nested_state,
	.get_nested_state_pages = pkvm_get_nested_state_pages,
	.write_log_dirty = pkvm_nested_write_pml_buffer,
};

struct kvm_x86_ops pkvm_host_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.check_processor_compatibility = pkvm_check_processor_compat,

	.hardware_unsetup = vmx_hardware_unsetup,

	.enable_virtualization_cpu = pkvm_enable_virtualization_cpu,
	.disable_virtualization_cpu = pkvm_disable_virtualization_cpu,
	.emergency_disable_virtualization_cpu = pkvm_emergency_disable_virtualization_cpu,

	.has_emulated_msr = pkvm_has_emulated_msr,

	.vm_size = sizeof(struct kvm_vmx),
	.vm_init = pkvm_vm_init,
	.vm_destroy = pkvm_vm_destroy,

	.vcpu_precreate = vmx_vcpu_precreate,
	.vcpu_create = pkvm_vcpu_create,
	.vcpu_free = pkvm_vcpu_free,
	.vcpu_reset = pkvm_vcpu_reset,

	.prepare_switch_to_guest = pkvm_prepare_switch_to_guest,
	.vcpu_load = pkvm_vcpu_load,
	.vcpu_put = pkvm_vcpu_put,

	.update_exception_bitmap = pkvm_update_exception_bitmap,
	.get_feature_msr = pkvm_get_feature_msr,
	.get_msr = pkvm_get_msr,
	.set_msr = pkvm_set_msr,
	.get_segment_base = pkvm_get_segment_base,
	.get_segment = pkvm_get_segment,
	.set_segment = pkvm_set_segment,
	.get_cpl = vmx_get_cpl,
	.get_cs_db_l_bits = vmx_get_cs_db_l_bits,
	.is_valid_cr0 = pkvm_is_valid_cr0,
	.set_cr0 = pkvm_set_cr0,
	.is_valid_cr4 = pkvm_is_valid_cr4,
	.set_cr4 = pkvm_set_cr4,
	.set_efer = pkvm_set_efer,
	.get_idt = pkvm_get_idt,
	.set_idt = pkvm_set_idt,
	.get_gdt = pkvm_get_gdt,
	.set_gdt = pkvm_set_gdt,
	.set_dr7 = pkvm_set_dr7,
	.sync_dirty_debug_regs = vmx_sync_dirty_debug_regs,
	.cache_reg = vmx_cache_reg,
	.get_rflags = pkvm_get_rflags,
	.set_rflags = pkvm_set_rflags,
	.get_if_flag = pkvm_get_if_flag,

	.flush_tlb_all = pkvm_flush_tlb_all,
	.flush_tlb_current = pkvm_flush_tlb_current,
	.flush_tlb_gva = pkvm_flush_tlb_gva,
	.flush_tlb_guest = pkvm_flush_tlb_guest,

	.vcpu_pre_run = vmx_vcpu_pre_run,
	.vcpu_run = pkvm_vcpu_run,
	.handle_exit = vmx_handle_exit,
	.skip_emulated_instruction = vmx_skip_emulated_instruction,
	.update_emulated_instruction = pkvm_update_emulated_instruction,
	.set_interrupt_shadow = pkvm_set_interrupt_shadow,
	.get_interrupt_shadow = pkvm_get_interrupt_shadow,
	.patch_hypercall = vmx_patch_hypercall,
	.inject_irq = pkvm_inject_irq,
	.inject_nmi = pkvm_inject_nmi,
	.inject_exception = vmx_inject_exception,
	.cancel_injection = vmx_cancel_injection,
	.interrupt_allowed = pkvm_interrupt_allowed,
	.nmi_allowed = pkvm_nmi_allowed,
	.get_nmi_mask = vmx_get_nmi_mask,
	.set_nmi_mask = vmx_set_nmi_mask,
	.enable_nmi_window = vmx_enable_nmi_window,
	.enable_irq_window = vmx_enable_irq_window,
	.update_cr8_intercept = vmx_update_cr8_intercept,

	.x2apic_icr_is_split = false,
	.set_virtual_apic_mode = vmx_set_virtual_apic_mode,
	.set_apic_access_page_addr = vmx_set_apic_access_page_addr,
	.refresh_apicv_exec_ctrl = vmx_refresh_apicv_exec_ctrl,
	.load_eoi_exitmap = vmx_load_eoi_exitmap,
	.apicv_pre_state_restore = vmx_apicv_pre_state_restore,
	.required_apicv_inhibits = VMX_REQUIRED_APICV_INHIBITS,
	.hwapic_irr_update = vmx_hwapic_irr_update,
	.hwapic_isr_update = vmx_hwapic_isr_update,
	.sync_pir_to_irr = vmx_sync_pir_to_irr,
	.deliver_interrupt = vmx_deliver_interrupt,
	.dy_apicv_has_pending_interrupt = pi_has_pending_interrupt,

	.set_tss_addr = vmx_set_tss_addr,
	.set_identity_map_addr = vmx_set_identity_map_addr,
	.get_mt_mask = vmx_get_mt_mask,

	.get_exit_info = vmx_get_exit_info,

	.vcpu_after_set_cpuid = pkvm_vcpu_after_set_cpuid,

	.has_wbinvd_exit = cpu_has_vmx_wbinvd_exit,

	.get_l2_tsc_offset = pkvm_get_l2_tsc_offset,
	.get_l2_tsc_multiplier = pkvm_get_l2_tsc_multiplier,
	.write_tsc_offset = vmx_write_tsc_offset,
	.write_tsc_multiplier = vmx_write_tsc_multiplier,

	.load_mmu_pgd = vmx_load_mmu_pgd,

	.check_intercept = pkvm_check_intercept,
	.handle_exit_irqoff = vmx_handle_exit_irqoff,

	.cpu_dirty_log_size = PML_ENTITY_NUM,
	.update_cpu_dirty_logging = vmx_update_cpu_dirty_logging,

	.nested_ops = &pkvm_nested_ops,

	.pi_update_irte = vmx_pi_update_irte,
	.pi_start_assignment = vmx_pi_start_assignment,

#ifdef CONFIG_X86_64
	.set_hv_timer = vmx_set_hv_timer,
	.cancel_hv_timer = vmx_cancel_hv_timer,
#endif

	.setup_mce = vmx_setup_mce,

#ifdef CONFIG_KVM_SMM
	.smi_allowed = pkvm_smi_allowed,
	.enter_smm = pkvm_enter_smm,
	.leave_smm = pkvm_leave_smm,
	.enable_smi_window = pkvm_enable_smi_window,
#endif

	.check_emulate_instruction = vmx_check_emulate_instruction,
	.apic_init_signal_blocked = pkvm_apic_init_signal_blocked,
	.migrate_timers = pkvm_migrate_timers,

	.msr_filter_changed = vmx_msr_filter_changed,
	.complete_emulated_msr = pkvm_complete_emulated_msr,

	.vcpu_deliver_sipi_vector = kvm_vcpu_deliver_sipi_vector,

	.get_untagged_addr = vmx_get_untagged_addr,
};

static struct kvm_pmc *pkvm_intel_rdpmc_ecx_to_pmc(struct kvm_vcpu *vcpu,
						   unsigned int idx, u64 *mask)
{
	return NULL;
}
static struct kvm_pmc *pkvm_intel_msr_idx_to_pmc(struct kvm_vcpu *vcpu, u32 msr) { return NULL; }
static bool pkvm_intel_is_valid_msr(struct kvm_vcpu *vcpu, u32 msr) { return false; }
static int pkvm_intel_pmu_get_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info) { return 1; }
static int pkvm_intel_pmu_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info) { return 1; }
static void pkvm_intel_pmu_refresh(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_init(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_reset(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_deliver_pmi(struct kvm_vcpu *vcpu) {}
static void pkvm_intel_pmu_cleanup(struct kvm_vcpu *vcpu) {}

static struct kvm_pmu_ops pkvm_intel_pmu_ops __initdata = {
	.rdpmc_ecx_to_pmc = pkvm_intel_rdpmc_ecx_to_pmc,
	.msr_idx_to_pmc = pkvm_intel_msr_idx_to_pmc,
	.is_valid_msr = pkvm_intel_is_valid_msr,
	.get_msr = pkvm_intel_pmu_get_msr,
	.set_msr = pkvm_intel_pmu_set_msr,
	.refresh = pkvm_intel_pmu_refresh,
	.init = pkvm_intel_pmu_init,
	.reset = pkvm_intel_pmu_reset,
	.deliver_pmi = pkvm_intel_pmu_deliver_pmi,
	.cleanup = pkvm_intel_pmu_cleanup,
	.EVENTSEL_EVENT = ARCH_PERFMON_EVENTSEL_EVENT,
	.MAX_NR_GP_COUNTERS = 0,
	.MIN_NR_GP_COUNTERS = 0,
};

struct kvm_x86_init_ops pkvm_host_init_ops __initdata = {
	.hardware_setup = vmx_hardware_setup,
	.handle_intel_pt_intr = NULL,

	.runtime_ops = &pkvm_host_x86_ops,
	.pmu_ops = &pkvm_intel_pmu_ops,
};
