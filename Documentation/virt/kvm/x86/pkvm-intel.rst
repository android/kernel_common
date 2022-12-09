.. SPDX-License-Identifier: GPL-2.0

pKVM on Intel Platform Introduction
===================================

Protected-KVM (pKVM) on Intel platform is designed as a thin hypervisor,
it wants to extend KVM supporting VMs isolated from the host.

The concept of pKVM is first introduced by Google for ARM platform
[1][2][3], which aims to extend Trust Execution Environment (TEE) from
ARM secure world to virtual machines (VMs). Such VMs are protected by the
pKVM from the host OS or other VMs accessing the payloads running inside
(so called protected VM). More details about the overall idea, design,
and motivations can be found in Will's talk at KVM forum 2020 [4].

There are similar use cases on x86 platforms requesting protected
environment which is isolated from host OS for confidential computing.
Meanwhile host OS still presents the primary user interface and people
will expect the same bare metal experience as before in terms of both
performance and functionalities (like rich-IO usages), so the host OS
is desired to remain the ability to manage all system resources. At
the same time, in order to mitigate the attack to the confidential
computing environment, the Trusted Computing Base (TCB) shall be
minimized.

HW solutions e.g. TDX [5] also exist to support above use cases. But
they are available only on very new platforms. Hence having a software
solution on massive existing platforms is also plausible.

pKVM has the merit of both providing an isolated environment for
protected VMs and also sustaining rich bare metal experiences as
expected by the host OS. This is achieved by creating a small
hypervisor below the host OS which contains only minimal
functionalities (e.g. VMX, EPT, IOMMU, etc.) for isolating protected
VMs from host OS and other VMs. In the meantime the host kernel still
remains access to most of the system resources and plays the role of
managing VM life cycles, allocating VM resources, etc. Existing KVM
module calls into the hypervisor (via emulation or enlightened PV ops)
to complete missing functionalities which have been moved downward.

      +--------------------+   +-----------------+
      |                    |   |                 |
      |     host VM        |   |  protected VM   |
      |    (act like       |   |                 |
      |   on bare metal)   |   |                 |
      |			   |   +-----------------+
      |                    +---------------------+
      |            +--------------------+        |
      |            | vVMX, vEPT, vIOMMU |        |
      |            +--------------------+        |
      +------------------------------------------+
      +------------------------------------------+
      |       pKVM (own VMX, EPT, IOMMU)         |
      +------------------------------------------+

[note: above figure is based on Intel terminologies]

The terminologies used in this document:

- host VM:     native Linux which boot pKVM then deprivilege to a VM
- protected VM: VM launched by host but protected by pKVM
- normal VM:    VM launched & protected by host

pKVM binary is compiled as an extension of KVM module, but resides in a
separate, dedicated memory section of the vmlinux image. It makes pKVM
easy to release and verified boot together with Linux kernel image. It
also means pKVM is a post-launched hypervisor since it's started by KVM
module.

ARM platform naturally supports different exception level (EL) and the
host kernel can be set to run at EL1 during the early boot stage before
launching pKVM hypervisor, so pKVM just needs to be installed to EL2.
On Intel platform, the host Linux kernel is originally running in VMX
root mode, then deprivileged to run into vmx non-root mode as a host VM,
whereas pKVM is kept running at VMX root mode. Comparing with pKVM on
ARM, pKVM on Intel platform needs this deprivilege stage to prepare and
setup VMX environment in VMX root mode.

As a hypervisor, pKVM on Intel platform leverages virtualization
technologies (see below) to guarantee the isolation among itself and low
privilege guests (include host Linux) on top of it:

 - pKVM manages CPU state/context switch between hypervisor and different
   guests. It's largely done by VMCS.

 - pKVM owns EPT page table to manage the GPA to HPA mapping of its host
   VM and guest VMs, which ensures they will not touch the hypervisor's
   memory and isolate among each other. It's similar to pKVM on ARM which
   owns stage-2 MMU page table to isolate memory among hypervisor, host,
   protected VMs and normal VMs. To allow host manage EPT or stage-2 page
   tables, pKVM can choose to provide either PV ops or emulation for these
   page tables. pKVM on ARM chose PV ops, which providing hypervisor calls
   (HVCs) in pKVM for stage-2 MMU page table changes. pKVM on Intel
   platform provides emulation for EPT page table management - this avoids
   the code changes in x86 KVM MMU.

 - pKVM owns IOMMU (VT-d for Intel platform and SMMU for ARM platform)
   to manage device DMA buffer mapping to isolate DMA access. To allow
   host manage IOMMU page tables, smilar to EPT/stage-2 page table
   management, PV ops or emulation method could be chosen. pKVM on ARM
   chose PV ops [6], while pKVM on Intel platform will use IOMMU
   emulation.

A topic in KVM forum 2022 about supporting TEE on x86 client platforms
with pKVM [7] may help you understand more details about the framework
of pKVM on Intel platforms and the deltas between pKVM on Intel and ARM
platforms.

Deprivilege Host OS
===================

The primary motivation of pKVM on Intel platform is to be able to protect
VM's memory from the host, which is the same as pKVM on ARM. To achieve
this, the pKVM hypervisor shall run at the higher privilege level, while
Linux host kernel shall run at lower privilege level, which allow the
isolation control from the pKVM hypervisor. On ARM platform with nvhe
architecture, the Linux kernel runs at EL1, and pKVM runs at EL2, so that
pKVM on ARM can use stage-2 MMU translation to isolate guest memory from
the host kernel. Similarly for Intel architecture, only pKVM hypervisor
code runs at vmx root mode and the Linux kernel should run at vmx non-root
mode. But the host Linux kernel boots and runs at the vmx root mode, so it
needs to be deprivileged to vmx non-root mode. After that, the host becomes
a VM and its code/data is untrusted to pKVM hypervisor. Based on above, pKVM
code for Intel platform is divided into two parts: the deprivilege code (at
arch/x86/kvm/vmx/pkvm/) and the hypervisor code (at arch/x86/kvm/vmx/pkvm/hyp/).
The deprivilege code is pKVM initialization code in Linux kernel which helps
Linux kernel to deprivilege itself and ensure pKVM hypervisor keep running at
high privilege level. Meanwhile the hypervisor code is pKVM hypervisor runtime
code which is independent, self-contained, running at vmx root mode and isolated
to host Linux kernel.

1. Basic common infrastructure
-------------------------------
As pKVM hypervisor is independent and isolated to host Linux, the memory
resource it used shall be reserved and maintained by itself. On ARM platform,
the memory used by pKVM is reserved during bootmem_init() from the memblocks,
and managed by pKVM through its own buddy allocator, which is pretty general
for Intel platform as well. So the memory reservation and buddy allocator is
stripped from pKVM on ARM to make it a common infrastructure, and move the
code to virt/kvm/pkvm/.

1) Memory Reservation
---------------------
The reserved memory size is calculated by pkvm_total_reserve_pages() which is
depending on the architecture. For Intel platform, pKVM reserves the memory
for its data structures, vmemmap metadata of buddy allocator, MMU of hypervisor,
EPT of the host VM, and shadow EPT of the guest. The reserved memory is
physically contiguous.

2) Buddy Allocator
------------------
The Buddy allocator is designed and implemented in pKVM on ARM platform [8]
and is used as a common infrastructure, which is a conventional 'buddy
allocator', working with page granularity. It allows allocating and free
physically contiguous pages from memory 'pools', with a guaranteed order
alignment in the PA space. Each page in a memory pool is associated with a
struct pkvm_page which holds the page's metadata, including its refcount, as
well as its current order, hence mimicking the kernel's buddy system in the
GFP infrastructure. The pkvm_page metadata are made accessible through a
pkvm_vmemmap, following the concept of SPARSE_VMEMMAP in the kernel.

Although buddy allocator is a common infrastructure, it may still need to use
some architecture-specific APIs, like spinlock and VA<->PA translations. These
are wrapped to general APIs, like pkvm_spin_lock, __pkvm_va(phys), __pkvm_pa(va)
with different architecture implementations in the back.

Buddy allocator will be used by pKVM hypervisor code to dynamically allocate
and free memory at the runtime.

2. Independent binary of pKVM hypervisor
----------------------------------------
As the Linux kernel runs at vmx non-root mode, its code/data is untrusted to
pKVM hypervisor. The symbols in Linux kernel address space cannot be used by
pKVM hypervisor. To build an independent pKVM hypervisor binary, introduced a
linker script to put the hypervisor code and data in separated sections. Doing
so can easily isolate all pKVM hypervisor's code/data memory from the host
Linux kernel. This is different with pKVM deprivilege code - such code only
executes for deprivilege but not at the hypervisor runtime, they do not need
to be an independent binary. So the deprivilege code is compiled as usual and
able to use Linux kernel symbols.

As pKVM hypervisor can only link to its symbols, while some common libraries
from Linux kernel are expected being used by pKVM hypervisor as well, so pull
them into pKVM's code section, e.g., memset, memcpy, find_bit etc..

To avoid symbol clashing between pKVM hypervisor code and Linux kernel,
added the prefix '__pkvm_' to all pKVM hypervisor's symbols. Doing so also
can help to catch the case that pKVM links symbols without '__pkvm_' prefix
at the building time. To reduce redundant code in pKVM, some of pKVM hypervisor
symbols may be used by the pKVM deprivilege code. As all the pKVM hypervisor
symbols are prefixed with '__pkvm_', it needs to explicitly add the prefix
'__pkvm_' when calls these symbols by the deprivilege code, which is implemented
by a simple macro pkvm_sym(symbol).

To simplify, the pKVM hypervisor build also removed ftrace, Shadow Call Stack,
CFI CFLAGS, and disabled stack protector. As pKVM hypervisor shouldn't export any
symbols, also disabled 'EXPORT_SYMBOL'.

3. pKVM Initialization
----------------------

With CONFIG_PKVM_INTEL=y, pKVM will be compiled into Linux kernel. During the
boot time, the Linux kernel reserves physical continuous memory according to the
size calculated by pkvm_total_reserve_pages() for pKVM hypervisor. The reserved
memory will be used as a memory pool for pKVM to dynamic allocate its own used
memory at the deprivilege time and runtime.

pKVM deprivilege code will start to run when loads the kvm-intel module, and
after finishing the deprivilege, pKVM hypervisor code runs in vmx root mode.
And the rest part of the Linux kernel is deprivileged to vmx non-root mode. Host
Linux must be trusted until pKVM deprivileged it, so CONFIG_PKVM_INTEL=y selects
kvm-intel as a built-in module, which can be loaded earlier than user space
booting, so that pKVM can start deprivilege earlier.

The buddy allocator will not be ready until pKVM hypervisor has set up the
pkvm_vmemmap. So before that, pKVM uses early_alloc mechanism to contiguously
allocate memory from the reserved area with holding a lock to avoid racing.
Unlike buddy allocator which can release the allocated memory through putting
the reference count in pkvm_vmemmap, early_alloc mechanism doesn't have
reference count so the memory allocated by early_alloc is not expected to be
released.

1) Allocate/Setup pkvm_hyp
--------------------------
pkvm_hyp is a data structure allocated by early_alloc at the deprivilege time.
It contains vmcs_config, vmx_capability, MMU/EPT capability, hypervisor MMU,
physical CPU instances, host VM vCPU instances, host VM EPT.

The vmcs_config and vmx_capability is set up with the mandatory capability like
EPT, shadow VMCS. To give the best performance to host VM, most of the IO/MSRs
accessing is configured as passthrough, as well as the interrupts and
exceptions. So almost all the IO devices(E.g., LAPIC/IOAPIC, serial port I/O,
all the PCI/PCIe devices) can be directly accessed by the host VM, and the
external interrupt can be directly injected to the host VM without causing any
vmexit. Only a few necessary vmexits can be triggered by the host VM, like
CPUID, CR accessing, intercepted MSRs. These setups will be used to configure
the VMCS later.

Unlike vmcs_config/vmx_capability structure in pkvm_hyp, the physical/virtual
CPU instances are defined as pointer array, and the instances are allocated by
early_alloc according to the real CPU number. This is due to the CPU number is
different from platform to platform, and cannot predefine data structure array
with the maximum CPU number CONFIG_NR_CPUS, which will waste a lot of memory.
So the instances are allocated according to the real CPU number of this platform
running with, and each CPU will have a physical CPU instance and a virtual CPU
instance.

The physical CPU instance stores the hypervisor's state, e.g., stack pages, GDT,
TSS, IDT, CR3. These states will be used to configure VMCS host state. As
mentioned in the above part, external interrupts will be directly injected to
the host VM, so the hypervisor will run with interrupt disabled and doesn't
handle any interrupt. Hypervisor also should not cause any exception at runtime,
so IDT is initialized with noop handlers for all the vectors except for NMI. NMI
is un-maskable so it may happen when hypervisor is running so a valid NMI handler
in hypervisor code is necessary.

The virtual CPU instance stores host vCPU states by using the VMX structure
vcpu_vmx. The VMCS pages and MSR bitmap page are also allocated through
early_alloc.

4. Deprivilege the Linux Kernel
--------------------------------

Deprivilege the Linux kernel will finally make it running at vmx non-root mode
on each CPU, and pKVM hypervisor code will run at vmx root mode. To achieve this,
each physical CPU needs to turn on vmx and vmlaunch to vmx non-root mode.

1) Setup VMCS
-------------
After vmx is on, each CPU can load and set up a VMCS. The VMCS setup is majorly
done for guest state, host state, and control states (execution control,
vmentry/vmexit controls).

The guest state is for the host VM. It is configured with the current native
platform states, including CR registers, segment registers and MSRs, so that the
Linux kernel can smoothly run in vmx non-root mode after deprivilege.

The host state is for the pKVM hypervisor. It is configured by using its own
GDT/IDT/TSS for segment registers, and reusing the CR registers and MSRs of
the current native platform. Reusing the Linux kernel's CR3 is temporary and
CR3 will be updated in the finalize phase when hypervisor's MMU page table is
ready.

The control state is configured according to the pkvm_hyp.vmcs_config, which
passthrough most of the IO/MSRs as well as interrupts and exceptions. Some
resources which are controlled by hypervisor need to be intercepted, like VMX
MSRs, CR4 VMXE bit. EPT is not enabled at this moment as the EPT page table is
created at the finalize phase by pKVM hypervisor code, so EPT will be updated
later, similar to CR3.

2) Deprivilege
--------------
After VMCS is setup, pKVM can start to deprivilege by executing vmlaunch on
each CPU. As the Linux kernel will start to run at the position after doing
vmlaunch, GUEST_RFLAGS/GUEST_RSP are configured to the current native rflags/rsp
registers and GUEST_RIP are set to the code next to the vmlaunch. Meanwhile,
HOST_RSP/HOST_RIP are also properly configured for running hypervisor vmexit
handlers. With these setups, after executing vmlaunch, the CPU enters vmx
non-root mode and jump to the place pointed by GUEST_RIP. At this point, the
Linux kernel runs at the vmx non-root mode.

[1]: https://lwn.net/Articles/836693/
[2]: https://lwn.net/Articles/837552/
[3]: https://lwn.net/Articles/895790/
[4]: https://kvmforum2020.sched.com/event/eE24/virtualization-for-the-masses-exposing-kvm-on-android-will-deacon-google
[5]: https://software.intel.com/content/www/us/en/develop/articles/intel-trust-domain-extensions.html
[6]: https://lore.kernel.org/linux-arm-kernel/20230201125328.2186498-1-jean-philippe@linaro.org/T/
[7]: https://kvmforum2022.sched.com/event/15jKc/supporting-tee-on-x86-client-platforms-with-pkvm-jason-chen-intel
[8]: https://lore.kernel.org/r/20210319100146.1149909-13-qperret@google.com
