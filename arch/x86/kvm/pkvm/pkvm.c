// SPDX-License-Identifier: GPL-2.0
#include <asm/processor.h>
#include <asm/kvm_pkvm.h>
#include "x86.h"
#include "pkvm.h"
#include "mmu.h"
#include "cpuid.h"
#include "fpu/fpu.h"
#include <asm/pkvm_spinlock.h>
//FIXME: clean up the header files
#include <vmx/pkvm/hyp/mem_protect.h>
#include <vmx/pkvm/hyp/memory.h>
#include <vmx/pkvm/hyp/trace.h>
#include <pkvm.h>

/*
 * FIXME: This was defined in kvm/mmu/mmu.c but as this file is not used for
 * adding cpu state protection, there is no equivalent mmu.c in the pkvm
 * hypervisor, define the tdp_enabled here to simplify.
 */
bool tdp_enabled = true;
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);
/*
 * In the pkvm hypervisor, the kvm_rebooting is never to be set as there is no
 * notification from the host about the system shutting down or rebooting.
 */
__visible bool kvm_rebooting;

size_t pkvm_vm_sz = sizeof(struct pkvm_vm);
size_t pkvm_vcpu_sz = sizeof(struct pkvm_vcpu);

static DECLARE_BITMAP(pkvm_vms_bitmap, MAX_PKVM_VMS);
static pkvm_spinlock_t pkvm_vms_lock = __PKVM_SPINLOCK_UNLOCKED;
static struct pkvm_vm_ref pkvm_vms_ref[MAX_PKVM_VMS];
struct pkvm_x86_ops pkvm_x86_ops __read_mostly;

static DEFINE_PER_CPU(union pkvm_pv_param *, pv_param);

#define this_pv_param(f)	(&this_cpu_read(pv_param)->f)

static void *donate_host_memory(unsigned long gpa, size_t size, bool clear)
{
	unsigned long pa = host_gpa2hpa(gpa);
	void *va;

	if (!VALID_PAGE(pa) || !PAGE_ALIGNED(pa) ||
	    !PAGE_ALIGNED(size) || !size)
		return NULL;

	if (__pkvm_host_donate_hyp(pa, size))
		return NULL;

	va = __pkvm_va(pa);
	if (clear)
		memset(va, 0, size);

	return va;
}

static int pkvm_enable_virtualization_cpu(unsigned long pv_param_pa)
{
	int r = kvm_arch_enable_virtualization_cpu();

	if (!r)
		/*
		 * TODO: Assume host is already share the pv_param structure
		 * with pkvm. Pin the pv_param_pa so that it won't be re-used
		 * as guest memory.
		 */
		this_cpu_write(pv_param, __pkvm_va(pv_param_pa));

	return r;
}

static void pkvm_disable_virtualization_cpu(void)
{
	kvm_arch_disable_virtualization_cpu();

	/* TODO: unpin the shared pv_param memory */
	this_cpu_write(pv_param, NULL);
}

#define HANDLE_OFFSET 1

static int idx_to_vm_handle(int idx)
{
	return idx + HANDLE_OFFSET;
}

static int vm_handle_to_idx(int handle)
{
	return handle - HANDLE_OFFSET;
}

static int allocate_pkvm_vm_handle(struct pkvm_vm *pkvm_vm)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	int idx;

	/*
	 * The pkvm_vm_handle is an int so cannot exceed the INT_MAX.
	 * Meanwhile pkvm_vm_handle will also be used as owner_id in
	 * the page state machine so it also cannot exceed the max
	 * owner_id.
	 */
	BUILD_BUG_ON(MAX_PKVM_VMS >
		     min(INT_MAX, ((1 << hweight_long(PKVM_INVALID_PTE_OWNER_MASK)) - 1)));

	pkvm_spin_lock(&pkvm_vms_lock);

	idx = find_next_zero_bit(pkvm_vms_bitmap, MAX_PKVM_VMS, 0);
	if (idx == MAX_PKVM_VMS) {
		pkvm_spin_unlock(&pkvm_vms_lock);
		return -ENOMEM;
	}
	__set_bit(idx, pkvm_vms_bitmap);

	to_kvm(pkvm_vm)->arch.pkvm.pkvm_vm_handle = idx_to_vm_handle(idx);
	pkvm_vm_ref = &pkvm_vms_ref[idx];
	pkvm_vm_ref->pkvm_vm = pkvm_vm;
	atomic_set(&pkvm_vm_ref->refcount, 1);

	pkvm_spin_unlock(&pkvm_vms_lock);
	return 0;
}

static struct pkvm_vm *free_pkvm_vm_handle(int handle)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	struct pkvm_vm *pkvm_vm = NULL;
	int idx;

	idx = vm_handle_to_idx(handle);
	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	pkvm_spin_lock(&pkvm_vms_lock);

	idx = array_index_nospec(idx, MAX_PKVM_VMS);
	pkvm_vm_ref = &pkvm_vms_ref[idx];
	if ((atomic_cmpxchg(&pkvm_vm_ref->refcount, 1, 0) != 1)) {
		pr_err("%s: VM%d is busy, refcount %d\n",
			__func__, handle, atomic_read(&pkvm_vm_ref->refcount));
		goto out;
	}

	pkvm_vm = pkvm_vm_ref->pkvm_vm;
	pkvm_vm_ref->pkvm_vm = NULL;

	__clear_bit(idx, pkvm_vms_bitmap);
out:
	pkvm_spin_unlock(&pkvm_vms_lock);
	return pkvm_vm;
}

static void teardown_donated_memory(struct pkvm_memcache *mc, void *addr, size_t size)
{
	size = PAGE_ALIGN(size);
	pkvm_clear_memory(addr, size);

	for (void *start = addr; start < addr + size; start += PAGE_SIZE)
		push_pkvm_memcache(mc, start, pkvm_virt_to_host_gpa);

	/*
	 * The sensitive data is already cleared at the beginning of this
	 * function. And then this memory range is used as memcache to store the
	 * physical addresses of the memory pages for the host to free. So the
	 * donation cannot clear the memory range.
	 */
	__pkvm_hyp_donate_host(pkvm_virt_to_phys(addr), size, false);
}

static int pkvm_vm_init(struct kvm *shared_kvm, unsigned long gpa, unsigned long pgd_gpa)
{
	unsigned long pkvm_vm_pa;
	struct pkvm_vm *pkvm_vm;
	struct kvm *kvm;
	size_t pa_size;
	int ret;

	pkvm_vm_pa = host_gpa2hpa(gpa);
	if (!PAGE_ALIGNED(pkvm_vm_pa))
		return -EINVAL;

	pa_size = PAGE_ALIGN(pkvm_vm_sz);
	if (__pkvm_host_donate_hyp(pkvm_vm_pa, pa_size))
		return -EINVAL;

	pkvm_vm = pkvm_phys_to_virt(pkvm_vm_pa);
	memset(pkvm_vm, 0, pa_size);

	pkvm_vm->size = pa_size;
	/*
	 * TODO: Assume host is already share the kvm structure
	 * (represented by shared_kvm) with pkvm. So just pin
	 * shared_kvm.
	 */
	pkvm_vm->shared_kvm = shared_kvm;
	pkvm_vm->lock = __PKVM_SPINLOCK_UNLOCKED;

	kvm = to_kvm(pkvm_vm);

	ret = kvm_arch_init_vm(kvm, pkvm_vm->shared_kvm->arch.vm_type);
	if (ret)
		goto undonate;

	ret = pkvm_vm_mmu_init(pkvm_vm, pgd_gpa);
	if (ret)
		goto vm_destroy;

	ret = allocate_pkvm_vm_handle(pkvm_vm);
	if (ret)
		goto mmu_destroy;

	init_pkvm_mmu_memcache(&pkvm_vm->shared_kvm->arch.pkvm.guest_mmu_teardown_mc);

	return kvm->arch.pkvm.pkvm_vm_handle;

mmu_destroy:
	pkvm_vm_mmu_destroy(pkvm_vm);
vm_destroy:
	kvm_x86_call(vm_destroy)(kvm);
undonate:
	__pkvm_hyp_donate_host(pkvm_vm_pa, pa_size, false);
	return ret;
}

static int pkvm_arch_vcpu_create(struct pkvm_vcpu *pkvm_vcpu, struct fpstate *fps)
{
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);
	int ret;

	/* Set cpu to -1 to indicate it is not loaded on any CPU */
	vcpu->cpu = -1;
	vcpu->kvm = to_kvm(pkvm_vcpu->pkvm_vm);
	vcpu->vcpu_id = pkvm_vcpu->shared_vcpu->vcpu_id;
	/*
	 * Set apic in vcpu->arch points to the apic in the shared vcpu
	 * to make the pkvm hypervisor knows if lapic_in_kernel() is true or
	 * not. The pkvm hypervisor should not use this as a normal memory.
	 */
	vcpu->arch.apic = pkvm_vcpu->shared_vcpu->arch.apic;
	vcpu->arch.apic_base = pkvm_vcpu->shared_vcpu->arch.apic_base;
	vcpu->arch.guest_fpu.fpstate = fps;

	vcpu->arch.mce_banks = (void *)pkvm_vcpu + pkvm_vcpu_sz;
	vcpu->arch.mci_ctl2_banks = (void *)vcpu->arch.mce_banks + MCE_BANKS_SIZE;

	ret = kvm_arch_vcpu_create(vcpu);
	if (ret)
		return ret;

	pkvm_vcpu_perf_init(vcpu);

	return 0;
}

static int attach_pkvm_vcpu_to_vm(struct pkvm_vm *pkvm_vm, struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm *kvm = to_kvm(pkvm_vm);
	int vcpu_handle;

	pkvm_spin_lock(&pkvm_vm->lock);

	if (kvm->created_vcpus == KVM_MAX_VCPUS) {
		pkvm_spin_unlock(&pkvm_vm->lock);
		return -EINVAL;
	}
	vcpu_handle = kvm->created_vcpus++;
	pkvm_vcpu->vcpu_idx = vcpu_handle;
	pkvm_vm->vcpus[vcpu_handle] = pkvm_vcpu;

	pkvm_spin_unlock(&pkvm_vm->lock);

	atomic_set(&pkvm_vm->vcpu_refs[vcpu_handle], 1);

	return vcpu_handle;
}

static struct pkvm_vcpu *detach_pkvm_vcpu_from_vm(struct pkvm_vm *pkvm_vm, int vcpu_handle)
{
	int refcount = atomic_cmpxchg(&pkvm_vm->vcpu_refs[vcpu_handle], 1, 0);
	struct pkvm_vcpu *pkvm_vcpu;

	if (refcount > 1) {
		/* The pkvm_vcpu is in use and cannot be detached . */
		pr_err("pkvm: VM%d vcpu%d is busy, refcount %d\n",
		       to_kvm(pkvm_vm)->arch.pkvm.pkvm_vm_handle,
		       vcpu_handle, refcount);
		return NULL;
	} else if (refcount == 0) {
		/* No pkvm_vcpu is attached. */
		return NULL;
	}

	BUG_ON(refcount != 1);

	pkvm_spin_lock(&pkvm_vm->lock);

	pkvm_vcpu = pkvm_vm->vcpus[vcpu_handle];
	pkvm_vm->vcpus[vcpu_handle] = NULL;

	pkvm_spin_unlock(&pkvm_vm->lock);

	return pkvm_vcpu;
}

struct pkvm_vm *get_pkvm_vm(int handle)
{
	int idx = vm_handle_to_idx(handle);
	struct pkvm_vm_ref *pkvm_vm_ref;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	idx = array_index_nospec(idx, MAX_PKVM_VMS);
	pkvm_vm_ref = &pkvm_vms_ref[idx];
	return atomic_inc_not_zero(&pkvm_vm_ref->refcount) ? pkvm_vm_ref->pkvm_vm : NULL;
}

void put_pkvm_vm(struct pkvm_vm *pkvm_vm)
{
	int idx = vm_handle_to_idx(to_kvm(pkvm_vm)->arch.pkvm.pkvm_vm_handle);
	struct pkvm_vm_ref *pkvm_vm_ref;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return;

	pkvm_vm_ref = &pkvm_vms_ref[idx];
	WARN_ON(atomic_dec_if_positive(&pkvm_vm_ref->refcount) <= 0);
}

static struct pkvm_vcpu *donate_pkvm_vcpu(unsigned long vcpu_gpa)
{
	size_t size = PAGE_ALIGN(pkvm_vcpu_sz + MCE_BANKS_SIZE + MCI_CTL2_BANKS_SIZE);
	struct pkvm_vcpu *pkvm_vcpu;

	pkvm_vcpu = donate_host_memory(vcpu_gpa, size, true);
	if (!pkvm_vcpu)
		return NULL;

	pkvm_vcpu->size = size;

	return pkvm_vcpu;
}

static struct fpstate *donate_fpu(unsigned long fpu_gpa, size_t size)
{
	struct fpstate *fps = donate_host_memory(fpu_gpa, size, true);

	if (!fps)
		return NULL;

	/*
	 * Although the fpstate size represents the size of the register memory,
	 * use this field to save the size of the fpstate memory to simplify the
	 * undonating, which is the only usage of the fpstate size field in the
	 * pkvm hypervisor.
	 */
	fps->size = size;
	return fps;
}

static int pkvm_vcpu_create(struct kvm_vcpu *shared_vcpu, unsigned long vcpu_gpa,
			    unsigned long fpu_gpa)
{
	struct pkvm_vcpu *pkvm_vcpu;
	struct pkvm_vm *pkvm_vm;
	struct kvm *shared_kvm;
	struct fpstate *fps;
	int ret;

	shared_kvm = kern_pkvm_va(shared_vcpu->kvm);
	pkvm_vm = get_pkvm_vm(shared_kvm->arch.pkvm.pkvm_vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	pkvm_vcpu = donate_pkvm_vcpu(vcpu_gpa);
	if (!pkvm_vcpu) {
		ret = -EINVAL;
		goto put_vm;
	}

	fps = donate_fpu(fpu_gpa, pkvm_guest_initial_fpstate_size(to_kvm(pkvm_vm)));
	if (!fps) {
		ret = -EINVAL;
		goto undonate_vcpu;
	}

	/*
	 * TODO: Assume host is already share the kvm_vcpu structure
	 * (represented by shared_vcpu) with pkvm. So just pin
	 * shared_vcpu. Unpin shared_vcpu when destroying
	 */
	pkvm_vcpu->shared_vcpu = shared_vcpu;

	pkvm_vcpu->pkvm_vm = pkvm_vm;
	ret = pkvm_arch_vcpu_create(pkvm_vcpu, fps);
	if (ret)
		goto undonate_fpu;

	ret = attach_pkvm_vcpu_to_vm(pkvm_vm, pkvm_vcpu);
	if (ret < 0)
		goto vcpu_destroy;

	put_pkvm_vm(pkvm_vm);

	return pkvm_vcpu->vcpu_idx;

vcpu_destroy:
	kvm_x86_call(vcpu_free)(to_kvm_vcpu(pkvm_vcpu));
undonate_fpu:
	__pkvm_hyp_donate_host(__pkvm_pa(fps), fps->size, false);
undonate_vcpu:
	__pkvm_hyp_donate_host(__pkvm_pa(pkvm_vcpu), pkvm_vcpu->size, false);
put_vm:
	put_pkvm_vm(pkvm_vm);
	return ret;
}

static int __pkvm_vcpu_free(struct pkvm_vm *pkvm_vm, int vcpu_handle)
{
	struct kvm_protected_vm *shared_pkvm;
	struct pkvm_vcpu *pkvm_vcpu;
	struct kvm_vcpu *vcpu;

	pkvm_vcpu = detach_pkvm_vcpu_from_vm(pkvm_vm, vcpu_handle);
	if (!pkvm_vcpu)
		return -EINVAL;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	kvm_x86_call(vcpu_free)(vcpu);

	shared_pkvm = &pkvm_vm->shared_kvm->arch.pkvm;

	pkvm_free_mmu_memcache(vcpu, &shared_pkvm->guest_mmu_teardown_mc);

	teardown_donated_memory(&shared_pkvm->teardown_mc,
				(void *)vcpu->arch.cpuid_entries,
				sizeof(struct kvm_cpuid_entry2) *
				vcpu->arch.cpuid_nent);
	teardown_donated_memory(&shared_pkvm->teardown_mc,
				(void *)vcpu->arch.guest_fpu.fpstate,
				vcpu->arch.guest_fpu.fpstate->size);
	teardown_donated_memory(&shared_pkvm->teardown_mc,
				(void *)pkvm_vcpu, pkvm_vcpu->size);

	/* TODO: unpin shared kvm_vcpu */

	return 0;
}

static int pkvm_vcpu_free(struct kvm_vcpu *shared_vcpu)
{
	struct kvm *shared_kvm = kern_pkvm_va(shared_vcpu->kvm);
	int vcpu_handle = shared_vcpu->arch.pkvm_vcpu.handle;
	struct pkvm_vm *pkvm_vm;
	int ret;

	if (!shared_kvm || vcpu_handle < 0 || vcpu_handle >= KVM_MAX_VCPUS)
		return -EINVAL;

	pkvm_vm = get_pkvm_vm(shared_kvm->arch.pkvm.pkvm_vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	vcpu_handle = array_index_nospec(vcpu_handle, KVM_MAX_VCPUS);
	ret = __pkvm_vcpu_free(pkvm_vm, vcpu_handle);

	put_pkvm_vm(pkvm_vm);
	return ret;
}

static int pkvm_vm_finalize(int handle)
{
	struct kvm *kvm, *shared_kvm;
	struct pkvm_vm *pkvm_vm;
	u64 pvmfw_load_addr;
	int ret = 0, i;

	pkvm_vm = get_pkvm_vm(handle);
	if (!pkvm_vm)
		return -EINVAL;

	kvm = to_kvm(pkvm_vm);
	shared_kvm = pkvm_vm->shared_kvm;

	if (!pkvm_is_protected_vm(kvm)) {
		ret = -EINVAL;
		goto put_pkvm_vm;
	}

	pkvm_spin_lock(&pkvm_vm->lock);

	if (kvm->arch.pkvm.finalized) {
		ret = -EBUSY;
		goto unlock;
	}

	pvmfw_load_addr = READ_ONCE(shared_kvm->arch.pkvm.pvmfw_load_addr);
	if (pvmfw_load_addr != INVALID_GPA) {
		if (!pvmfw_present || U64_MAX - pvmfw_load_addr < pvmfw_size) {
			ret = -EINVAL;
			goto unlock;
		}
		kvm->arch.pkvm.pvmfw_load_addr = pvmfw_load_addr;
	}

	kvm->arch.bsp_vcpu_id = shared_kvm->arch.bsp_vcpu_id;

	/*
	 * Make sure to update pvmfw_load_addr and bsp_vcpu_id values before
	 * updating the primary vCPU's mp_state, i.e. before allowing the
	 * primary vCPU to run. Paired with smp_rmb() in pkvm_vcpu_run().
	 */
	smp_wmb();

	for (i = 0; i < kvm->created_vcpus; i++) {
		struct kvm_vcpu *vcpu;

		if (WARN_ON_ONCE(!pkvm_vm->vcpus[i]))
			continue;

		vcpu = to_kvm_vcpu(pkvm_vm->vcpus[i]);
		if (vcpu->vcpu_id == kvm->arch.bsp_vcpu_id)
			WRITE_ONCE(vcpu->arch.mp_state, KVM_MP_STATE_RUNNABLE);

		/*
		 * FIXME: temporarily allow secondary vCPUs to run as well
		 * until we implement a guest PV mechanism to let the pVM itself
		 * allow a vCPU to run.
		 */
		WRITE_ONCE(vcpu->arch.mp_state, KVM_MP_STATE_RUNNABLE);
	}

	kvm->arch.pkvm.finalized = true;
	shared_kvm->arch.pkvm.finalized = true;
unlock:
	pkvm_spin_unlock(&pkvm_vm->lock);
put_pkvm_vm:
	put_pkvm_vm(pkvm_vm);
	return ret;
}

static void pkvm_vm_destroy(int handle)
{
	struct kvm_protected_vm *shared_pkvm;
	struct pkvm_vm *pkvm_vm;
	int i;

	pkvm_vm = free_pkvm_vm_handle(handle);
	if (!pkvm_vm)
		return;

	pkvm_vm->is_dying = true;

	shared_pkvm = &pkvm_vm->shared_kvm->arch.pkvm;

	/*
	 * Normally all the created pkvm_vcpus should have been freed already
	 * by the vcpu_free PV interface. In case any pkvm_vcpu is still not
	 * freed, try to free it here.
	 */
	for (i = 0; i < to_kvm(pkvm_vm)->created_vcpus; i++)
		__pkvm_vcpu_free(pkvm_vm, i);

	pkvm_vm_mmu_destroy(pkvm_vm);

	kvm_arch_destroy_vm(to_kvm(pkvm_vm));
	teardown_donated_memory(&shared_pkvm->teardown_mc,
				(void *)pkvm_vm, pkvm_vm->size);

	/* TODO: unpin shared_kvm */
}

struct pkvm_vcpu *get_pkvm_vcpu(int vm_handle, int vcpu_handle)
{
	struct pkvm_vm *pkvm_vm;

	if (vcpu_handle < 0 || vcpu_handle >= KVM_MAX_VCPUS)
		return NULL;

	pkvm_vm = get_pkvm_vm(vm_handle);
	if (!pkvm_vm)
		return NULL;

	vcpu_handle = array_index_nospec(vcpu_handle, KVM_MAX_VCPUS);
	if (atomic_inc_not_zero(&pkvm_vm->vcpu_refs[vcpu_handle]))
		return pkvm_vm->vcpus[vcpu_handle];

	put_pkvm_vm(pkvm_vm);
	return NULL;
}

void put_pkvm_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	WARN_ON(atomic_dec_if_positive(&pkvm_vcpu->pkvm_vm->vcpu_refs[pkvm_vcpu->vcpu_idx]) <= 0);

	put_pkvm_vm(pkvm_vcpu->pkvm_vm);
}

struct pkvm_vcpu *get_pkvm_vcpu_via_shared(struct kvm_vcpu *shared_vcpu)
{
	struct pkvm_vcpu *pkvm_vcpu;
	struct kvm *shared_kvm;

	/*
	 * FIXME: Any check neeeds to be performed before accessing
	 * the shared_vcpu?
	 */
	shared_kvm = kern_pkvm_va(shared_vcpu->kvm);
	pkvm_vcpu = get_pkvm_vcpu(shared_kvm->arch.pkvm.pkvm_vm_handle,
				  shared_vcpu->arch.pkvm_vcpu.handle);
	if (!pkvm_vcpu)
		return NULL;

	if (pkvm_vcpu->shared_vcpu == shared_vcpu)
		return pkvm_vcpu;

	put_pkvm_vcpu(pkvm_vcpu);
	return NULL;
}

static bool is_kvm_vcpu_accessible(struct kvm_vcpu *vcpu, unsigned long fn)
{
	/*
	 * Determine if the vcpu is accessible for a specific PV interface.
	 * For the npVM, the vcpu is always accessible.
	 */
	if (!pkvm_is_protected_vcpu(vcpu))
		return true;

	switch (fn) {
	/*
	 * FIXME:
	 * Any security concern to allow the host using below PV interfaces?
	 * 1) __pkvm__get_nmi_mask: the host KVM needs these for nmi injection.
	 *
	 * TODO: Add protection for below PV interfaces:
	 *	__pkvm__write_tsc_offset
	 *	__pkvm__write_tsc_multiplier
	 */
	case __pkvm__vcpu_after_set_cpuid:
	case __pkvm__vcpu_reset:
	case __pkvm__get_segment_base:
	case __pkvm__get_segment:
	case __pkvm__set_segment:
	case __pkvm__set_cr0:
	case __pkvm__set_cr4:
	case __pkvm__set_msr:
	case __pkvm__get_msr:
	case __pkvm__set_efer:
	case __pkvm__get_idt:
	case __pkvm__set_idt:
	case __pkvm__get_gdt:
	case __pkvm__set_gdt:
	case __pkvm__get_rflags:
	case __pkvm__set_rflags:
	case __pkvm__set_dr7:
	case __pkvm__flush_tlb_all:
	case __pkvm__flush_tlb_current:
	case __pkvm__flush_tlb_gva:
	case __pkvm__flush_tlb_guest:
	case __pkvm__set_interrupt_shadow:
	case __pkvm__get_interrupt_shadow:
	case __pkvm__inject_exception:
	case __pkvm__set_nmi_mask:
	case __pkvm__post_set_cr3:
	case __pkvm__cache_reg:
	case __pkvm__update_exception_bitmap:
	case __pkvm__vcpu_add_fpstate:
		/*
		 * As the host needs to pre-configure the pVM's vcpu state for
		 * booting, the protection is only enforced by the pkvm hypervisor
		 * once the vcpu has started running.
		 *
		 * If the pVM runs with pvmfw, the hypervisor will enforce
		 * some of the vcpu's initial state before the first vcpu starts
		 * running, in pkvm_vcpu_pvmfw_entry_init(). However, before
		 * that we don't know yet if the pVM will run with pvmfw or not,
		 * since the host VMM may issue the ioctl enabling pvmfw either
		 * before or after using any of the above PV interfaces.
		 *
		 * For secondary vCPUs, also allow the host to pre-configure the
		 * initial state of the vcpu, even though the hypervisor itself
		 * will enforce the initial state before the secondary vcpu starts
		 * running, in pkvm_vcpu_ap_entry_init(), discarding whatever
		 * the host has pre-configured. This is just for simplicity, to
		 * let the host KVM code work as usual.
		 */
		return !kvm_vcpu_has_run(vcpu);
	default:
		break;
	}

	return true;
}

static struct pkvm_vcpu *load_pkvm_vcpu(struct kvm_vcpu *shared_vcpu, unsigned long fn)
{
	struct pkvm_vcpu *pkvm_vcpu = get_pkvm_vcpu_via_shared(shared_vcpu);
	struct kvm_vcpu *vcpu;

	if (!pkvm_vcpu)
		return NULL;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	if (!is_kvm_vcpu_accessible(vcpu, fn))
		goto out;

	/*
	 * Except the vcpu_load PV interface, the other vcpu accessing PV
	 * interfaces requires the vcpu to be loaded on this CPU.
	 */
	if (vcpu->cpu != raw_smp_processor_id() &&
	    !(vcpu->cpu == -1 && fn == __pkvm__vcpu_load))
		goto out;

	pkvm_x86_call(switch_to_guest_vcpu)(vcpu);

	if (pkvm_is_protected_vcpu(vcpu) && fn == __pkvm__vcpu_run) {
		struct kvm_vcpu *hvcpu = this_cpu_read(host_vcpu);

		/*
		 * A protected vcpu is going to run. Before returning back to
		 * the host, request to flush the L1D to hide the contents of
		 * the protected vcpu from the host.
		 */
		hvcpu->arch.l1tf_flush_l1d = true;
	}

	return pkvm_vcpu;
out:
	put_pkvm_vcpu(pkvm_vcpu);
	return NULL;
}

static void unload_pkvm_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	if (!pkvm_vcpu)
		return;

	pkvm_x86_call(switch_to_host_vcpu)(this_cpu_read(host_vcpu));

	put_pkvm_vcpu(pkvm_vcpu);
}

static void pkvm_vcpu_load(struct pkvm_vcpu *pkvm_vcpu, int cpu)
{
	struct kvm_vcpu *vcpu;
	int loaded_cpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(cpu != raw_smp_processor_id()))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	loaded_cpu = cmpxchg(&vcpu->cpu, -1, cpu);
	if (loaded_cpu == -1) {
		/*
		 * Get the pkvm_vcpu to prevent it from being freed via the
		 * vcpu_free PV interface while it is still loaded. If the
		 * obtained pkvm_vcpu is not the same as the original one, it
		 * must be a pkvm bug.
		 */
		BUG_ON(pkvm_vcpu != get_pkvm_vcpu(vcpu->kvm->arch.pkvm.pkvm_vm_handle,
						  pkvm_vcpu->vcpu_idx));
	} else if (WARN_ON_ONCE(loaded_cpu != cpu)) {
		/* Already loaded on another CPU */
		return;
	}

	/*
	 * Flush the L1D if a guest vcpu is going to run, to hide the L1D
	 * contents of the host/pkvm from the guest.
	 */
	vcpu->arch.l1tf_flush_l1d = true;

	kvm_x86_call(vcpu_load)(vcpu, cpu);

	/* Save host pkru register if supported */
	vcpu->arch.host_pkru = read_pkru();
}

static void pkvm_vcpu_put(struct pkvm_vcpu *pkvm_vcpu)
{
	int cpu = raw_smp_processor_id(), loaded_cpu;
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	loaded_cpu = cmpxchg(&vcpu->cpu, cpu, -1);
	/* Only the CPU which has loaded this vcpu can do a put */
	if (WARN_ON_ONCE(loaded_cpu != cpu))
		return;

	kvm_x86_call(vcpu_put)(vcpu);

	/*
	 * Put this pkvm_vcpu to allow it to be freed via the vcpu_free PV
	 * interface.
	 */
	put_pkvm_vcpu(pkvm_vcpu);
}

static void pkvm_vcpu_pvmfw_entry_init(struct kvm_vcpu *vcpu)
{
	struct kvm_segment seg;
	struct desc_ptr dt;
	unsigned long cr0;

	/* pvmfw entry point is at the beginning of the pvmfw image. */
	kvm_rip_write(vcpu, vcpu->kvm->arch.pkvm.pvmfw_load_addr);

	/* pvmfw starts in 32-bit protected mode with paging disabled. */
	cr0 = kvm_read_cr0(vcpu);
	cr0 |= X86_CR0_PE;
	cr0 &= ~X86_CR0_PG;
	kvm_x86_call(set_cr0)(vcpu, cr0);

	/* Set up flat 4GB segments. */
	memset(&seg, 0, sizeof(seg));
	seg.limit = 0xffffffff;
	seg.type = 0xb;
	seg.present = 1;
	seg.db = 1;	/* 32-bit segment */
	seg.s = 1;
	seg.g = 1;
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_CS);
	seg.type = 0x3;
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_DS);
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_ES);
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_FS);
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_GS);
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_SS);

	memset(&dt, 0, sizeof(dt));

	/*
	 * Initially hardware will use the cached segment descriptors we've set up
	 * above, so GDT in memory does not matter, until the guest reloads
	 * a segment register. Set the initial GDTR to an invalid GDT, so that
	 * if pvmfw accidentally reloads a segment register before it has set up
	 * its own GDT, it generates a #GP.
	 */
	kvm_x86_call(set_gdt)(vcpu, &dt);

	/* Similarly for TSS */
	memset(&seg, 0, sizeof(seg));
	seg.type = 0xb;
	seg.present = 1;
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_TR);

	/* ...and LDT */
	memset(&seg, 0, sizeof(seg));
	seg.unusable = 1;
	kvm_x86_call(set_segment)(vcpu, &seg, VCPU_SREG_LDTR);

	/*
	 * Set the initial IDTR to an invalid IDT, so that any early exception
	 * (before pvmfw sets up its own IDT) results in a triple fault.
	 */
	kvm_x86_call(set_idt)(vcpu, &dt);
}

static void pkvm_vcpu_ap_entry_init(struct kvm_vcpu *vcpu)
{
	/* FIXME: temporary, until guest kernel is updated to use PKVM_GHC_START_CPU. */
	if (!to_pkvm_vcpu(vcpu)->sipi_vector)
		return;

	kvm_vcpu_reset(vcpu, true);
	kvm_vcpu_deliver_sipi_vector(vcpu, to_pkvm_vcpu(vcpu)->sipi_vector);
}

static void pkvm_vcpu_update_state_from_host(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *shared_vcpu = pkvm_vcpu->shared_vcpu;
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);

	if (!pkvm_is_protected_vcpu(vcpu)) {
		/*
		 * Make sure the RSP/RIP in shared_vcpu are aligned with the
		 * private vcpu if they are not dirty.
		 */
		if (kvm_register_is_dirty(shared_vcpu, VCPU_REGS_RSP))
			kvm_register_mark_dirty(vcpu, VCPU_REGS_RSP);
		else
			shared_vcpu->arch.regs[VCPU_REGS_RSP] = kvm_rsp_read(vcpu);
		if (kvm_register_is_dirty(shared_vcpu, VCPU_REGS_RIP))
			kvm_register_mark_dirty(vcpu, VCPU_REGS_RIP);
		else
			shared_vcpu->arch.regs[VCPU_REGS_RIP] = kvm_rip_read(vcpu);
		/* Update the npVM's GPRs from the host */
		memcpy(vcpu->arch.regs, shared_vcpu->arch.regs,
		       NR_VCPU_REGS * sizeof(*vcpu->arch.regs));

		/* Update the debug registers from the host */
		memcpy(vcpu->arch.db, shared_vcpu->arch.db,
		       ARRAY_SIZE(vcpu->arch.db) * sizeof(*vcpu->arch.db));
		memcpy(vcpu->arch.eff_db, shared_vcpu->arch.eff_db,
		       ARRAY_SIZE(vcpu->arch.db) * sizeof(*vcpu->arch.db));
		vcpu->arch.dr6 = shared_vcpu->arch.dr6;
		vcpu->arch.dr7 = shared_vcpu->arch.dr7;
		if (vcpu->guest_debug & KVM_GUESTDBG_USE_HW_BP)
			vcpu->arch.guest_debug_dr7 = shared_vcpu->arch.guest_debug_dr7;
		if (vcpu->guest_debug & KVM_GUESTDBG_SINGLESTEP)
			vcpu->arch.singlestep_rip = shared_vcpu->arch.singlestep_rip;
	} else if (unlikely(!kvm_vcpu_has_run(vcpu) && kvm_vcpu_is_reset_bsp(vcpu))) {
		/*
		 * Allow the host VMM to set the initial values of most GPRs
		 * to let it pass boot information to the pVM payload and/or
		 * to pvmfw using various boot protocols, e.g. in RSI with the
		 * Linux/x86 boot protocol or in RAX/RBX with Multiboot.
		 */
		kvm_rax_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RAX]);
		kvm_rbx_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RBX]);
		kvm_rcx_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RCX]);
		kvm_rdx_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RDX]);
		kvm_rsi_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RSI]);
		kvm_rdi_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RDI]);
		if (kvm_register_is_dirty(shared_vcpu, VCPU_REGS_RSP))
			kvm_rsp_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RSP]);
		kvm_rbp_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RBP]);
		kvm_r8_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_R8]);
		kvm_r9_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_R9]);
		kvm_r10_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_R10]);
		kvm_r11_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_R11]);
		kvm_r12_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_R12]);
		kvm_r13_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_R13]);
		kvm_r14_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_R14]);

		/* Reserve R15 for pKVM for future extensions. */
		kvm_r15_write(vcpu, 0);

		/*
		 * If the host VMM boots the pVM directly, without pvmfw,
		 * let it set the boot entry address.
		 */
		if (!pkvm_vcpu_is_pvmfw_bsp(vcpu) &&
		    kvm_register_is_dirty(shared_vcpu, VCPU_REGS_RIP))
			kvm_rip_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RIP]);

		return;
	} else if (unlikely(!kvm_vcpu_has_run(vcpu) && !kvm_vcpu_is_reset_bsp(vcpu))) {
		/*
		 * FIXME: temporarily let the host set the initial RIP for
		 * secondary vCPUs for INIT/SIPI emulation, until we implement
		 * a guest PV mechanism for secondary vCPUs startup.
		 */
		kvm_rip_write(vcpu, shared_vcpu->arch.regs[VCPU_REGS_RIP]);
		return;
	}

	pkvm_x86_call(sync_vcpu_state_post_switch)(pkvm_vcpu);
}

static void pkvm_vcpu_share_state_to_host(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *shared_vcpu = pkvm_vcpu->shared_vcpu;
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);

	if (!pkvm_is_protected_vcpu(vcpu)) {
		/* Make sure the RSP/RIP in private vcpu are up-to-date */
		if (!kvm_register_is_available(vcpu, VCPU_REGS_RSP))
			kvm_rsp_read(vcpu);
		if (!kvm_register_is_available(vcpu, VCPU_REGS_RIP))
			kvm_rip_read(vcpu);

		/*
		 * Share the npVM's GPRs/EFER/CR0/CR4 to the host which may be
		 * used by the host to handle vmexit.
		 *
		 * In particular, EFER/CR0/CR4 need to be shared when making
		 * HOST_INIT_MMU or HOST_RESET_MMU requests to the host,
		 * to let the host update the guest stage-1 MMU info which is
		 * needed for instruction emulation for npVM.
		 */
		memcpy(shared_vcpu->arch.regs, vcpu->arch.regs,
		       NR_VCPU_REGS * sizeof(*vcpu->arch.regs));
		kvm_register_mark_available(shared_vcpu, VCPU_REGS_RSP);
		kvm_register_mark_available(shared_vcpu, VCPU_REGS_RIP);
		shared_vcpu->arch.cr0 = kvm_read_cr0(vcpu);
		kvm_register_mark_available(shared_vcpu, VCPU_EXREG_CR0);
		shared_vcpu->arch.cr4 = kvm_read_cr4(vcpu);
		kvm_register_mark_available(shared_vcpu, VCPU_EXREG_CR4);
		shared_vcpu->arch.efer = vcpu->arch.efer;

		/*
		 * Share the exception information to the host if there is any
		 */
		if (vcpu->arch.exception.pending || vcpu->arch.exception.injected) {
			shared_vcpu->arch.exception = vcpu->arch.exception;
			kvm_clear_exception_queue(vcpu);
		}

		/* Share the debug registers to the host */
		memcpy(shared_vcpu->arch.db, vcpu->arch.db,
		       ARRAY_SIZE(vcpu->arch.db) * sizeof(*vcpu->arch.db));
		memcpy(shared_vcpu->arch.eff_db, vcpu->arch.eff_db,
		       ARRAY_SIZE(vcpu->arch.db) * sizeof(*vcpu->arch.db));
		shared_vcpu->arch.dr6 = vcpu->arch.dr6;
		shared_vcpu->arch.dr7 = vcpu->arch.dr7;
	}

	pkvm_x86_call(sync_vcpu_state_pre_switch)(pkvm_vcpu);
}

static unsigned long pkvm_vcpu_run(struct pkvm_vcpu *pkvm_vcpu, bool force_immediate_exit)
{
	struct kvm_vcpu *vcpu;
	unsigned long reqs;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return 0;

	vcpu = to_kvm_vcpu(pkvm_vcpu);

	if (unlikely(pkvm_is_protected_vcpu(vcpu) && !kvm_vcpu_has_run(vcpu))) {
		if (READ_ONCE(vcpu->arch.mp_state) != KVM_MP_STATE_RUNNABLE)
			return 0;

		/*
		 * Ensure that pvmfw_load_addr and bsp_vcpu_id (for primary vCPU)
		 * or sipi_vector (for secondary vCPU) are read after mp_state,
		 * so they are read with up-to-date values. Paired with smp_wmb()
		 * in pkvm_vm_finalize() and in pkvm_start_secondary_vcpu().
		 */
		smp_rmb();

		if (pkvm_vcpu_is_pvmfw_bsp(vcpu))
			pkvm_vcpu_pvmfw_entry_init(vcpu);
		else if (!kvm_vcpu_is_reset_bsp(vcpu))
			pkvm_vcpu_ap_entry_init(vcpu);
	}

	/*
	 * Flush predictor when switching from host VM to pVM to prevent host VM
	 * from attacking pVM. This is not needed if switch from host VM to npVM
	 * as host VM is in npVM's trust boundary.
	 */
	if (pkvm_is_protected_vcpu(vcpu))
		indirect_branch_prediction_barrier();

	pkvm_vcpu_update_state_from_host(pkvm_vcpu);

	reqs = kvm_vcpu_enter_guest(vcpu, force_immediate_exit);

	pkvm_vcpu_share_state_to_host(pkvm_vcpu);

	/*
	 * Flush predictor when switching from any guest VM (either npVM or pVM)
	 * to host VM, to prevent guest VM from attacking host VM.
	 */
	indirect_branch_prediction_barrier();

	return reqs;
}

static unsigned long pkvm_vcpu_after_set_cpuid(struct pkvm_vcpu *pkvm_vcpu,
					       unsigned long gpa,
					       size_t size)
{
	struct kvm_cpuid_entry2 *new, *old;
	int new_nent, old_nent;
	struct kvm_vcpu *vcpu;
	void *free;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return gpa;

	free = new = donate_host_memory(gpa, size, false);
	if (!new)
		return gpa;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	old = vcpu->arch.cpuid_entries;
	old_nent = vcpu->arch.cpuid_nent;

	new_nent = size / sizeof(struct kvm_cpuid_entry2);
	if (!kvm_set_cpuid(vcpu, new, new_nent) && (vcpu->arch.cpuid_entries == new)) {
		/*
		 * New physical page is consumed. Tear down the old cpuid
		 * entry memory pages if there is.
		 */
		if (old) {
			size = sizeof(struct kvm_cpuid_entry2) * old_nent;
			/*
			 * Store the free memory size at the beginning for the
			 * host to retrieve.
			 */
			*(size_t *)old = size;
			free = old;
		} else {
			/* No old cpuid entry memory pages to free. */
			return INVALID_PAGE;
		}
	}

	/*
	 * Undonate the physical pages which are going to be freed by the host.
	 * There is no need to clear these pages for npVM. For pVM, it is also
	 * not necessary as this is the point before the pVM begins execution.
	 * So the contents of these pages remain unchanged and reflect what the
	 * host has constructed. Meanwhile the size may be stored in the memory
	 * for the host to use, so also cannot be cleared.
	 */
	__pkvm_hyp_donate_host(__pkvm_pa(free), size, false);

	return __pkvm_pa(free);
}

static void pkvm_reset_vcpu(struct pkvm_vcpu *pkvm_vcpu, bool init_event)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_vcpu_reset(to_kvm_vcpu(pkvm_vcpu), init_event);
}

static u64 pkvm_get_segment_base(struct pkvm_vcpu *pkvm_vcpu, int seg)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return 0;

	return kvm_x86_call(get_segment_base)(to_kvm_vcpu(pkvm_vcpu), seg);
}

static void pkvm_get_segment(struct pkvm_vcpu *pkvm_vcpu, struct kvm_segment *var, int seg)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(var != this_pv_param(seg)))
		return;

	kvm_x86_call(get_segment)(to_kvm_vcpu(pkvm_vcpu), var, seg);
}

static void pkvm_set_segment(struct pkvm_vcpu *pkvm_vcpu, struct kvm_segment *var, int seg)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(var != this_pv_param(seg)))
		return;

	kvm_x86_call(set_segment)(to_kvm_vcpu(pkvm_vcpu), var, seg);
}

static void pkvm_set_cr0(struct pkvm_vcpu *pkvm_vcpu, unsigned long cr0)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(set_cr0)(to_kvm_vcpu(pkvm_vcpu), cr0);
}

static void pkvm_set_cr4(struct pkvm_vcpu *pkvm_vcpu, unsigned long cr4)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(set_cr4)(to_kvm_vcpu(pkvm_vcpu), cr4);
}

static int pkvm_set_msr(struct pkvm_vcpu *pkvm_vcpu, struct msr_data *msr)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return -EINVAL;

	if (WARN_ON_ONCE(msr != this_pv_param(msr)))
		return -EINVAL;

	return kvm_x86_call(set_msr)(to_kvm_vcpu(pkvm_vcpu), msr);
}

static int pkvm_get_msr(struct pkvm_vcpu *pkvm_vcpu, struct msr_data *msr)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return -EINVAL;

	if (WARN_ON_ONCE(msr != this_pv_param(msr)))
		return -EINVAL;

	return kvm_x86_call(get_msr)(to_kvm_vcpu(pkvm_vcpu), msr);
}

static int pkvm_set_efer(struct pkvm_vcpu *pkvm_vcpu, u64 efer)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return -EINVAL;

	return kvm_x86_call(set_efer)(to_kvm_vcpu(pkvm_vcpu), efer);
}

static void pkvm_access_idt_gdt(struct pkvm_vcpu *pkvm_vcpu, struct desc_ptr *desc,
				bool set, bool idt)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(desc != this_pv_param(desc)))
		return;

	if (idt) {
		if (set)
			kvm_x86_call(set_idt)(to_kvm_vcpu(pkvm_vcpu), desc);
		else
			kvm_x86_call(get_idt)(to_kvm_vcpu(pkvm_vcpu), desc);
	} else {
		if (set)
			kvm_x86_call(set_gdt)(to_kvm_vcpu(pkvm_vcpu), desc);
		else
			kvm_x86_call(get_gdt)(to_kvm_vcpu(pkvm_vcpu), desc);
	}
}

static void pkvm_set_dr7(struct pkvm_vcpu *pkvm_vcpu, unsigned long val)
{
	unsigned long dr7 = val;
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	kvm_x86_call(set_dr7)(vcpu, dr7);
	vcpu->arch.switch_db_regs &= ~KVM_DEBUGREG_BP_ENABLED;
	if (dr7 & DR7_BP_EN_MASK)
		vcpu->arch.switch_db_regs |= KVM_DEBUGREG_BP_ENABLED;
}

static unsigned long pkvm_get_rflags(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return 0;

	return kvm_x86_call(get_rflags)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_set_rflags(struct pkvm_vcpu *pkvm_vcpu, unsigned long val)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(set_rflags)(to_kvm_vcpu(pkvm_vcpu), val);
}

static void pkvm_flush_tlb_all(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(flush_tlb_all)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_flush_tlb_current(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(flush_tlb_current)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_flush_tlb_gva(struct pkvm_vcpu *pkvm_vcpu, gva_t addr)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(flush_tlb_gva)(to_kvm_vcpu(pkvm_vcpu), addr);
}

static void pkvm_flush_tlb_guest(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(flush_tlb_guest)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_set_interrupt_shadow(struct pkvm_vcpu *pkvm_vcpu, int mask)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(set_interrupt_shadow)(to_kvm_vcpu(pkvm_vcpu), mask);
}

static u32 pkvm_get_interrupt_shadow(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return 0;

	return kvm_x86_call(get_interrupt_shadow)(to_kvm_vcpu(pkvm_vcpu));
}

static int pkvm_complete_emulated_msr(struct pkvm_vcpu *pkvm_vcpu, int err)
{
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return 0;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	if (!pkvm_is_protected_vcpu(vcpu))
		return kvm_x86_call(complete_emulated_msr)(vcpu, err);

	/*
	 * Record the error code from the host for the emulated MSR
	 * rather than completing the emulation via
	 * kvm_x86_call(complete_emulated_msr), so that to prevent the
	 * host from injecting exception or skipping instructions for
	 * the pVM. The msr emulation will be completed before entering the guest.
	 */
	pkvm_vcpu->host_emulated_msr_err = err;
	return 1;
}

static int __pkvm_interrupt_allowed(struct pkvm_vcpu *pkvm_vcpu, bool for_injection)
{
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);

	if (!for_injection ||
	    (!kvm_event_needs_reinjection(vcpu) &&
	     !vcpu->arch.exception.pending &&
	     !pkvm_vcpu->host_emulated_msr_err))
		return kvm_x86_call(interrupt_allowed)(vcpu, for_injection);

	return -EBUSY;
}

static int pkvm_interrupt_allowed(struct pkvm_vcpu *pkvm_vcpu, bool for_injection)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return -EBUSY;

	return __pkvm_interrupt_allowed(pkvm_vcpu, for_injection);
}

static int __pkvm_nmi_allowed(struct pkvm_vcpu *pkvm_vcpu, bool for_injection)
{
	struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);

	if (!for_injection ||
	    (!kvm_event_needs_reinjection(vcpu) &&
	     !vcpu->arch.exception.pending &&
	     !pkvm_vcpu->host_emulated_msr_err))
		return kvm_x86_call(nmi_allowed)(vcpu, for_injection);

	return -EBUSY;
}

static int pkvm_nmi_allowed(struct pkvm_vcpu *pkvm_vcpu, bool for_injection)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return -EINVAL;

	return __pkvm_nmi_allowed(pkvm_vcpu, for_injection);
}

static void pkvm_inject_irq(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu, *shared_vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(__pkvm_interrupt_allowed(pkvm_vcpu, true) <= 0))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	shared_vcpu = pkvm_vcpu->shared_vcpu;
	vcpu->arch.interrupt.soft = shared_vcpu->arch.interrupt.soft;
	vcpu->arch.interrupt.nr = shared_vcpu->arch.interrupt.nr;

	kvm_x86_call(inject_irq)(vcpu, false);
}

static void pkvm_inject_nmi(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(__pkvm_nmi_allowed(pkvm_vcpu, true) <= 0))
		return;

	kvm_x86_call(inject_nmi)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_inject_exception(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu, *shared_vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	if (WARN_ON_ONCE(pkvm_is_protected_vcpu(vcpu)))
		return;

	shared_vcpu = pkvm_vcpu->shared_vcpu;
	vcpu->arch.exception = shared_vcpu->arch.exception;

	kvm_x86_call(inject_exception)(vcpu);
}

static void pkvm_cancel_injection(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu, *shared_vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	kvm_x86_call(cancel_injection)(vcpu);

	shared_vcpu = pkvm_vcpu->shared_vcpu;
	if (vcpu->arch.nmi_injected) {
		shared_vcpu->arch.nmi_injected = true;
		vcpu->arch.nmi_injected = false;
	} else if (vcpu->arch.interrupt.injected) {
		kvm_queue_interrupt(shared_vcpu, vcpu->arch.interrupt.nr,
				    vcpu->arch.interrupt.soft);
		kvm_clear_interrupt_queue(vcpu);
	} else if (!pkvm_is_protected_vcpu(vcpu) && vcpu->arch.exception.injected) {
		/*
		 * For the pVM, the exception can only be injected and canceled
		 * by the pkvm hypervisor.
		 * For the npVM, the exception can be injected and caceled by
		 * both sides.
		 */
		shared_vcpu->arch.exception = vcpu->arch.exception;
		kvm_clear_exception_queue(vcpu);
	}
}

static bool pkvm_get_nmi_mask(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return false;

	return kvm_x86_call(get_nmi_mask)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_set_nmi_mask(struct pkvm_vcpu *pkvm_vcpu, bool masked)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(set_nmi_mask)(to_kvm_vcpu(pkvm_vcpu), masked);
}

static void pkvm_enable_nmi_window(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(enable_nmi_window)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_enable_irq_window(struct pkvm_vcpu *pkvm_vcpu)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(enable_irq_window)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_update_cr8_intercept(struct pkvm_vcpu *pkvm_vcpu, int tpr, int irr)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(update_cr8_intercept)(to_kvm_vcpu(pkvm_vcpu), tpr, irr);
}

static void pkvm_set_virtual_apic_mode(struct pkvm_vcpu *pkvm_vcpu, u64 apic_base)
{
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	vcpu->arch.apic_base = apic_base;

	kvm_x86_call(set_virtual_apic_mode)(vcpu);
}

static void pkvm_refresh_apicv_exec_ctrl(struct pkvm_vcpu *pkvm_vcpu, bool apicv_active)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(refresh_apicv_exec_ctrl)(to_kvm_vcpu(pkvm_vcpu));
}

static void pkvm_load_eoi_exitmap(struct pkvm_vcpu *pkvm_vcpu, u64 *eoi_exit_bitmap)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	if (WARN_ON_ONCE(eoi_exit_bitmap != this_pv_param(eoi_exit_bitmap[0])))
		return;

	kvm_x86_call(load_eoi_exitmap)(to_kvm_vcpu(pkvm_vcpu), eoi_exit_bitmap);
}

static void pkvm_hwapic_irr_update(struct pkvm_vcpu *pkvm_vcpu, int max_irr)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(hwapic_irr_update)(to_kvm_vcpu(pkvm_vcpu), max_irr);
}

static void pkvm_hwapic_isr_update(struct pkvm_vcpu *pkvm_vcpu, int max_isr)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(hwapic_isr_update)(to_kvm_vcpu(pkvm_vcpu), max_isr);
}

static void pkvm_write_tsc_offset(struct pkvm_vcpu *pkvm_vcpu, u64 tsc_offset)
{
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	vcpu->arch.l1_tsc_offset = tsc_offset;
	vcpu->arch.tsc_offset = tsc_offset;
	kvm_x86_call(write_tsc_offset)(vcpu);
}

static void pkvm_write_tsc_multiplier(struct pkvm_vcpu *pkvm_vcpu, u64 ratio)
{
	struct kvm_vcpu *vcpu;

	if (!kvm_caps.has_tsc_control)
		return;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	vcpu->arch.l1_tsc_scaling_ratio = ratio;
	vcpu->arch.tsc_scaling_ratio = ratio;
	kvm_x86_call(write_tsc_multiplier)(vcpu);
}

static void pkvm_post_set_cr3(struct pkvm_vcpu *pkvm_vcpu, unsigned long cr3)
{
	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	kvm_x86_call(post_set_cr3)(to_kvm_vcpu(pkvm_vcpu), cr3);
}

static void pkvm_load_mmu_pgd(struct pkvm_vcpu *pkvm_vcpu, hpa_t root_hpa, int root_level)
{
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);

	kvm_x86_call(load_mmu_pgd)(vcpu, vcpu->arch.mmu->root.hpa,
				   vcpu->arch.mmu->root_role.level);
}

static void pkvm_setup_mce(struct pkvm_vcpu *pkvm_vcpu, u64 mcg_cap)
{
	unsigned bank_num = mcg_cap & 0xff, bank;
	struct kvm_vcpu *vcpu;

	if (!bank_num || bank_num > KVM_MAX_MCE_BANKS)
		return;

	if (mcg_cap & ~(kvm_caps.supported_mce_cap | 0xff | 0xff0000))
		return;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	vcpu->arch.mcg_cap = mcg_cap;

	/* Init IA32_MCG_CTL to all 1s */
	if (mcg_cap & MCG_CTL_P)
		vcpu->arch.mcg_ctl = ~(u64)0;
	/* Init IA32_MCi_CTL to all 1s, IA32_MCi_CTL2 to all 0s */
	for (bank = 0; bank < bank_num; bank++) {
		vcpu->arch.mce_banks[bank*4] = ~(u64)0;
		if (mcg_cap & MCG_CMCI_P)
			vcpu->arch.mci_ctl2_banks[bank] = 0;
	}

	kvm_x86_call(setup_mce)(vcpu);
}

static unsigned long pkvm_cache_reg(struct pkvm_vcpu *pkvm_vcpu, enum kvm_reg reg)
{
	struct kvm_vcpu *vcpu, *shared_vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return 0;

	vcpu = to_kvm_vcpu(pkvm_vcpu);

	kvm_x86_call(cache_reg)(vcpu, reg);

	shared_vcpu = pkvm_vcpu->shared_vcpu;

	switch (reg) {
	case VCPU_REGS_RSP:
		shared_vcpu->arch.regs[VCPU_REGS_RSP] = vcpu->arch.regs[VCPU_REGS_RSP];
		break;
	case VCPU_REGS_RIP:
		shared_vcpu->arch.regs[VCPU_REGS_RIP] = vcpu->arch.regs[VCPU_REGS_RIP];
		break;
	case VCPU_EXREG_PDPTR: {
		struct kvm_mmu *shared_mmu = shared_vcpu->arch.walk_mmu;
		struct kvm_mmu *mmu = vcpu->arch.walk_mmu;

		shared_mmu->pdptrs[0] = mmu->pdptrs[0];
		shared_mmu->pdptrs[1] = mmu->pdptrs[1];
		shared_mmu->pdptrs[2] = mmu->pdptrs[2];
		shared_mmu->pdptrs[3] = mmu->pdptrs[3];

		break;
	}
	case VCPU_EXREG_CR0:
		shared_vcpu->arch.cr0 = vcpu->arch.cr0;
		break;
	case VCPU_EXREG_CR3:
		shared_vcpu->arch.cr3 = vcpu->arch.cr3;
		break;
	case VCPU_EXREG_CR4:
		shared_vcpu->arch.cr4 = vcpu->arch.cr4;
		break;
	default:
		pr_err("pkvm: Unsupported register %d\n", reg);
		break;
	}

	return 0;
}

static void pkvm_update_cpuid_runtime(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu, *shared_vcpu;
	u64 old_apic_base;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	shared_vcpu = pkvm_vcpu->shared_vcpu;

	old_apic_base = vcpu->arch.apic_base;
	vcpu->arch.apic_base = shared_vcpu->arch.apic_base;

	if ((old_apic_base ^ vcpu->arch.apic_base) & MSR_IA32_APICBASE_ENABLE)
		kvm_update_cpuid_runtime(vcpu);
}

static void pkvm_update_exception_bitmap(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu, *shared_vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	shared_vcpu = pkvm_vcpu->shared_vcpu;
	vcpu->guest_debug = shared_vcpu->guest_debug;

	kvm_x86_call(update_exception_bitmap)(vcpu);
}

static unsigned long pkvm_vcpu_add_fpstate(struct pkvm_vcpu *pkvm_vcpu,
					   unsigned long gpa, size_t size)
{
	struct fpstate *new, *old;
	struct kvm_vcpu *vcpu;

	if (WARN_ON_ONCE(!pkvm_vcpu))
		return gpa;

	vcpu = to_kvm_vcpu(pkvm_vcpu);
	/* Expect the host to use this PV interface for pVM only. */
	if (!pkvm_is_protected_vcpu(vcpu))
		return gpa;

	old = vcpu->arch.guest_fpu.fpstate;
	/*
	 * Reuse the existing fpstate memory if it's sufficiently large. At this
	 * stage, we can't determine whether the new fpstate size matches the
	 * vCPUID or not, because that check only occurs when the host calls
	 * __pkvm__vcpu_after_set_cpuid to update the vCPUID. If the new fpstate
	 * size is smaller than what the new vCPUID requires, the vCPUID won't
	 * be updated. Therefore, ensuring the new fpstate size is at least as
	 * large as the previous one allows continued support for this scenario.
	 */
	if (old && old->size >= size)
		return gpa;

	new = donate_fpu(gpa, size);
	if (!new)
		return gpa;

	vcpu->arch.guest_fpu.fpstate = new;
	pkvm_init_guest_fpu(&vcpu->arch.guest_fpu);
	/*
	 * As only the pVM will be here, set the fpstate confidential
	 * unconditionally
	 */
	fpstate_set_confidential(&vcpu->arch.guest_fpu);

	if (!old)
		return INVALID_PAGE;

	size = old->size;
	/*
	 * Store the size in the beginning of the memory page to be freed, which
	 * the host will use to determine how much memory to free.
	 */
	*(size_t *)old = size;

	/*
	 * Undonate the physical pages which are going to be freed by the host.
	 * There is no need to clear these pages for npVM. For pVM, it is also
	 * not necessary as this is the point before the pVM begins execution.
	 * So the FPU state of pVM is not stored in these memory pages yet.
	 * Meanwhile the size is stored in the memory, so also cannot be cleared.
	 */
	__pkvm_hyp_donate_host(__pkvm_pa(old), size, false);

	return __pkvm_pa(old);
}

static unsigned long pkvm_vcpu_handle_kvm_call(unsigned long fn,
					       struct kvm_vcpu *shared_vcpu,
					       unsigned long p2, unsigned  long p3)
{
	struct pkvm_vcpu *pkvm_vcpu;
	unsigned long ret = 0;

	pkvm_vcpu = load_pkvm_vcpu(shared_vcpu, fn);

	switch (fn) {
	case __pkvm__vcpu_load:
		pkvm_vcpu_load(pkvm_vcpu, (int)p2);
		break;
	case __pkvm__vcpu_put:
		pkvm_vcpu_put(pkvm_vcpu);
		break;
	case __pkvm__vcpu_run:
		ret = pkvm_vcpu_run(pkvm_vcpu, (bool)p2);
		break;
	case __pkvm__vcpu_after_set_cpuid:
		ret = pkvm_vcpu_after_set_cpuid(pkvm_vcpu, p2, (size_t)p3);
		break;
	case __pkvm__vcpu_reset:
		pkvm_reset_vcpu(pkvm_vcpu, (bool)p2);
		break;
	case __pkvm__get_segment_base:
		ret = pkvm_get_segment_base(pkvm_vcpu, (int)p2);
		break;
	case __pkvm__get_segment:
		pkvm_get_segment(pkvm_vcpu, (struct kvm_segment *)kern_pkvm_va((void *)p2),
				 (int)p3);
		break;
	case __pkvm__set_segment:
		pkvm_set_segment(pkvm_vcpu, (struct kvm_segment *)kern_pkvm_va((void *)p2),
				 (int)p3);
		break;
	case __pkvm__set_cr0:
		pkvm_set_cr0(pkvm_vcpu, (unsigned long)p2);
		break;
	case __pkvm__set_cr4:
		pkvm_set_cr4(pkvm_vcpu, (unsigned long)p2);
		break;
	case __pkvm__set_msr:
		ret = pkvm_set_msr(pkvm_vcpu, (struct msr_data *)kern_pkvm_va((void *)p2));
		break;
	case __pkvm__get_msr:
		ret = pkvm_get_msr(pkvm_vcpu, (struct msr_data *)kern_pkvm_va((void *)p2));
		break;
	case __pkvm__set_efer:
		ret = pkvm_set_efer(pkvm_vcpu, (u64)p2);
		break;
	case __pkvm__get_idt:
		pkvm_access_idt_gdt(pkvm_vcpu, (struct desc_ptr *)kern_pkvm_va((void *)p2),
				    false, true);
		break;
	case __pkvm__set_idt:
		pkvm_access_idt_gdt(pkvm_vcpu, (struct desc_ptr *)kern_pkvm_va((void *)p2),
				    true, true);
		break;
	case __pkvm__get_gdt:
		pkvm_access_idt_gdt(pkvm_vcpu, (struct desc_ptr *)kern_pkvm_va((void *)p2),
				    false, false);
		break;
	case __pkvm__set_gdt:
		pkvm_access_idt_gdt(pkvm_vcpu, (struct desc_ptr *)kern_pkvm_va((void *)p2),
				    true, false);
		break;
	case __pkvm__set_dr7:
		pkvm_set_dr7(pkvm_vcpu, p2);
		break;
	case __pkvm__get_rflags:
		ret = pkvm_get_rflags(pkvm_vcpu);
		break;
	case __pkvm__set_rflags:
		pkvm_set_rflags(pkvm_vcpu, p2);
		break;
	case __pkvm__flush_tlb_all:
		pkvm_flush_tlb_all(pkvm_vcpu);
		break;
	case __pkvm__flush_tlb_current:
		pkvm_flush_tlb_current(pkvm_vcpu);
		break;
	case __pkvm__flush_tlb_gva:
		pkvm_flush_tlb_gva(pkvm_vcpu, (gva_t)p2);
		break;
	case __pkvm__flush_tlb_guest:
		pkvm_flush_tlb_guest(pkvm_vcpu);
		break;
	case __pkvm__set_interrupt_shadow:
		pkvm_set_interrupt_shadow(pkvm_vcpu, (int)p2);
		break;
	case __pkvm__get_interrupt_shadow:
		ret = pkvm_get_interrupt_shadow(pkvm_vcpu);
		break;
	case __pkvm__complete_emulated_msr:
		ret = pkvm_complete_emulated_msr(pkvm_vcpu, (int)p2);
		break;
	case __pkvm__interrupt_allowed:
		ret = pkvm_interrupt_allowed(pkvm_vcpu, (bool)p2);
		break;
	case __pkvm__nmi_allowed:
		ret = pkvm_nmi_allowed(pkvm_vcpu, (bool)p2);
		break;
	case __pkvm__inject_irq:
		pkvm_inject_irq(pkvm_vcpu);
		break;
	case __pkvm__inject_nmi:
		pkvm_inject_nmi(pkvm_vcpu);
		break;
	case __pkvm__inject_exception:
		pkvm_inject_exception(pkvm_vcpu);
		break;
	case __pkvm__cancel_injection:
		pkvm_cancel_injection(pkvm_vcpu);
		break;
	case __pkvm__get_nmi_mask:
		ret = pkvm_get_nmi_mask(pkvm_vcpu);
		break;
	case __pkvm__set_nmi_mask:
		pkvm_set_nmi_mask(pkvm_vcpu, (bool)p2);
		break;
	case __pkvm__enable_nmi_window:
		pkvm_enable_nmi_window(pkvm_vcpu);
		break;
	case __pkvm__enable_irq_window:
		pkvm_enable_irq_window(pkvm_vcpu);
		break;
	case __pkvm__update_cr8_intercept:
		pkvm_update_cr8_intercept(pkvm_vcpu, (int)p2, (int)p3);
		break;
	case __pkvm__set_virtual_apic_mode:
		pkvm_set_virtual_apic_mode(pkvm_vcpu, (u64)p2);
		break;
	case __pkvm__refresh_apicv_exec_ctrl:
		pkvm_refresh_apicv_exec_ctrl(pkvm_vcpu, (bool)p2);
		break;
	case __pkvm__load_eoi_exitmap:
		pkvm_load_eoi_exitmap(pkvm_vcpu, (u64 *)p2);
		break;
	case __pkvm__hwapic_irr_update:
		pkvm_hwapic_irr_update(pkvm_vcpu, (int)p2);
		break;
	case __pkvm__hwapic_isr_update:
		pkvm_hwapic_isr_update(pkvm_vcpu, (int)p2);
		break;
	case __pkvm__write_tsc_offset:
		pkvm_write_tsc_offset(pkvm_vcpu, (u64)p2);
		break;
	case __pkvm__write_tsc_multiplier:
		pkvm_write_tsc_multiplier(pkvm_vcpu, (u64)p2);
		break;
	case __pkvm__post_set_cr3:
		pkvm_post_set_cr3(pkvm_vcpu, p2);
		break;
	case __pkvm__load_mmu_pgd:
		pkvm_load_mmu_pgd(pkvm_vcpu, (hpa_t)p2, (int)p3);
		break;
	case __pkvm__setup_mce:
		pkvm_setup_mce(pkvm_vcpu, (u64)p2);
		break;
	case __pkvm__cache_reg:
		ret = pkvm_cache_reg(pkvm_vcpu, (enum kvm_reg)p2);
		break;
	case __pkvm__update_cpuid_runtime:
		pkvm_update_cpuid_runtime(pkvm_vcpu);
		break;
	case __pkvm__update_exception_bitmap:
		pkvm_update_exception_bitmap(pkvm_vcpu);
		break;
	case __pkvm__vcpu_add_fpstate:
		ret = pkvm_vcpu_add_fpstate(pkvm_vcpu, p2, (size_t)p3);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	unload_pkvm_vcpu(pkvm_vcpu);

	return ret;
}

unsigned long handle_kvm_call(unsigned long fn, unsigned long p1,
			      unsigned long p2, unsigned long p3,
			      unsigned long p4, unsigned long p5)
{
	unsigned long ret = 0;

	switch (fn) {
	case __pkvm__enable_virtualization_cpu:
		ret = pkvm_enable_virtualization_cpu(p1);
		break;
	case __pkvm__disable_virtualization_cpu:
		pkvm_disable_virtualization_cpu();
		break;
	case __pkvm__check_processor_compatibility:
		ret = kvm_x86_call(check_processor_compatibility)();
		break;
	case __pkvm__vm_init:
		ret = pkvm_vm_init((struct kvm *)kern_pkvm_va((void *)p1), p2, p3);
		break;
	case __pkvm__vm_finalize:
		ret = pkvm_vm_finalize((int)p1);
		break;
	case __pkvm__vm_destroy:
		pkvm_vm_destroy((int)p1);
		ret = 0;
		break;
	case __pkvm__vm_mmu_map:
		ret = pkvm_vm_mmu_map((struct kvm_vcpu *)kern_pkvm_va((void *)p1),
				      p2, p3, p4, p5);
		break;
	case __pkvm__vm_mmu_unmap:
		ret = pkvm_vm_mmu_unmap((int)p1, p2, p3);
		break;
	case __pkvm__vm_mmu_age:
		ret = pkvm_vm_mmu_age((int)p1, p2, p3, p4);
		break;
	case __pkvm__vcpu_create:
		ret = pkvm_vcpu_create((struct kvm_vcpu *)kern_pkvm_va((void *)p1), p2, p3);
		break;
	case __pkvm__vcpu_free:
		ret = pkvm_vcpu_free((struct kvm_vcpu *)kern_pkvm_va((void *)p1));
		break;
	default:
		ret = pkvm_vcpu_handle_kvm_call(fn, (struct kvm_vcpu *)kern_pkvm_va((void *)p1),
						p2, p3);
		break;
	}

	return ret;
}

void pkvm_x86_ops_init(struct pkvm_x86_ops *ops)
{
	memcpy(&pkvm_x86_ops, ops, sizeof(struct pkvm_x86_ops));
}

int pkvm_walk_each_vm(pkvm_vm_func_t func, void *arg)
{
	struct pkvm_vm *vm;
	int i, ret = 0;

	pkvm_spin_lock(&pkvm_vms_lock);

	for (i = 0; i < MAX_PKVM_VMS; i++) {
		vm = pkvm_vms_ref[i].pkvm_vm;
		if (!vm)
			continue;
		ret = func(vm, arg);
		if (ret)
			break;
	}

	pkvm_spin_unlock(&pkvm_vms_lock);

	return ret;
}

int pkvm_start_secondary_vcpu(struct pkvm_vm *pkvm_vm, u32 apic_id,
			      unsigned long start_ip)
{
	struct kvm *kvm = to_kvm(pkvm_vm);
	int ret = -EINVAL;
	int i;

	if (!pkvm_is_protected_vm(kvm))
		return -EINVAL;

	if (start_ip & ~0xff000)
		return -EFAULT;

	pkvm_spin_lock(&pkvm_vm->lock);

	for (i = 0; i < kvm->created_vcpus; i++) {
		struct pkvm_vcpu *pkvm_vcpu = pkvm_vm->vcpus[i];
		struct kvm_vcpu *vcpu = to_kvm_vcpu(pkvm_vcpu);

		if (vcpu->vcpu_id != apic_id)
			continue;

		if (kvm_vcpu_is_reset_bsp(vcpu)) {
			ret = -EINVAL;
			break;
		}

		if (!lapic_in_kernel(vcpu)) {
			ret = -EOPNOTSUPP;
			break;
		}

		pkvm_vcpu->sipi_vector = start_ip >> 12;
		/*
		 * Update sipi_vector before updating mp_state. Paired with
		 * smp_rmb() in pkvm_vcpu_run().
		 */
		smp_wmb();
		WRITE_ONCE(vcpu->arch.mp_state, KVM_MP_STATE_RUNNABLE);

		ret = 0;
		break;
	}

	pkvm_spin_unlock(&pkvm_vm->lock);

	return ret;
}
