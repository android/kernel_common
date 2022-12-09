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

3) Finalize Phase
-----------------
Although the Linux kernel now runs in vmx non-root mode, pKVM hypervisor is
not fully ready yet as MMU/EPT still need to be updated to guarantee the
isolation between pKVM hypervisor and the Linux kernel. Currently, the host
VM and the hypervisor are using the same CR3, without EPT enabled. So after
vmlaunch, each CPU will use a vmcall to enter vmx root mode to trigger pKVM
hypervisor to complete the last step of deprivilege, which is to finalize the
deprivilege.

The finalize vmcall takes the struct pkvm_section as input parameters, which
contains the range of the reserved memory and hypervisor's code/data sections.
The reserved memory is divided into several parts through early_alloc mechanism:
#1 pkvm_hyp data structures; #2 vmemmap metadata of buddy allocator; #3
hypervisor MMU pages; #4 host EPT pages; #5 shadow EPT pages (Note: part#1 is
already allocated before deprivilege, and the reset parts should not overlap
with part#1). Then hypervisor will set up the MMU/EPT with the divided memory
pages.

To enable the buddy allocator for a more flexible memory management, the vmemmap
metadata should be mapped in hypervisor's MMU first. So creating hypervisor's
MMU is the first thing to do after dividing the reserved memory. To simplify,
the MMU is created by mapping all the memblocks with kernel direct mapping
VA, and hypervisor's code/data sections with symbol VA. The vmemmap metadata is
mapped with the VA started from 0. Once all the required mappings are ready,
hypervisor can update its CR3 register with the new MMU page table. And after
that, hypervisor runs with its own CR3. With buddy allocator enabled, hypervisor
page-table manage framework can be used to dynamically manage the map/unmap for
hypervisor MMU and host VM's EPT. The page-table management is introduced in the
next section.

To guarantee the isolation, hypervisor set up EPT for host VM. The EPT is
identical mapped for all the memblocks. As the MMIO is usually out of the range
of the memblocks, also identical maps all the possible holes between each
memblock. However, some MMIO may live in the high-end address which is difficult
to be covered by mapping these holes, so hypervisor still needs to handle such
EPT violation at the runtime. With EPT, hypervisor can be isolated from the
host VM. The memory which is not expected to be accessed by host VM will be
unmapped from EPT in the finalize phase, like reserved memory and hypervisor's
code/data sections.

In the end of finalize phase, hypervisor code also initializes nested related
data, like shadow vmcs fields, emulated vmcs fields and shadow EPT pages pool.

Although each CPU will execute the finalize vmcall, only the first finalize
vmcall needs to divide reserved memory and set up the buddy allocator/MMU/EPT
as these are onetime jobs. Once these are done, the other finalize vmcalls
on the other CPUs only need to do per-CPU stuff: switching CR3 and enabling
EPT.

* Page-table management
-----------------------

As talked above, pKVM hypervisor finally needs to manage page tables for its
MMU, host VM EPT, and shadow EPT for guest VMs. To help supporting these
different page tables, pKVM provides a general page table walker framework.
Such framework provides interface for different operations like pgtable_ops
and mm_ops. The pgtable_ops provide operations for page table management, like
set page table entries, check a page table entry is present or whether it is a
leaf, or get entry size per page table level etc. Meanwhile the mm_ops provide
page table related mm operations, like page allocation, PV translation, flush
tlb etc. MMU and EPT can have different implementation for pgtable_ops & mm_ops,
thus they can use same page table walker framework to manage their page tables.

5. Isolated pKVM hypervisor
---------------------------

In the end of host deprivilege, pKVM hypervisor runs as an independent binary
with its own MMU page table. Host VM runs with EPT enabled, which unmaps the
pKVM hypervisor's code/data sections, as well as the reserved memory. With
this, accessing any pKVM hypervisor's memory from host VM will cause EPT
violation to the hypervisor, which guarantees the pKVM hypervisor is isolated
from host VM.


VMX Emulation (Shadow VMCS)
===========================

Host VM wants the capability to run its guest, it needs VMX support.

pKVM is designed to emulate VMX for host VM based on shadow vmcs.
This requires "VMCS shadowing" feature support in VMX secondary
processor-based VM-Execution controls field [9].

    +--------------------+   +-----------------+
    |     host VM        |   |   guest VM      |
    |                    |   |                 |
    |         +--------+ |   |                 |
    |         | vmcs12 | |   |                 |
    |         +--------+ |   |                 |
    +--------------------+   +-----------------+
    +------------------------------------------+       +---------+
    |     +--------+           +--------+      |       | shadow  |
    |     | vmcs01 |           | vmcs02 +------+---+-->|  vcpu   |
    |     +--------+           +--------+      |   |   |  state  |
    |                      +---------------+   |   |   +---------+
    |                      | cached_vmcs12 +---+---+
    | pKVM                 +---------------+   |
    +------------------------------------------+

"VMCS shadowing" use a shadow vmcs page (vmcs02) to cache vmcs fields
accessing from host VM through VMWRITE/VMREAD, avoid causing vmexit.
The fields cached in vmcs02 is pre-defined by VMREAD/VMWRITE bitmap.
Meanwhile for other fields not in VMREAD/VMWRITE bitmap, accessing from
host VM cause VMREAD/VMWRITE vmexit, pKVM need to cache them in another
place - cached_vmcs12 is introduced for this purpose.

The vmcs02 page in root mode is kept in the structure shadow_vcpu_state,
which allocated then donated from host VM when it initializes vcpus for
its launched guest (nested). Same for field of cached_vmcs12.

pKVM use vmcs02 with two purposes, one is mentioned above, using it
as the shadow vmcs page of nested guest when host VM program its vmcs
fields. The other one is using it as ordinary (or active) vmcs for the
same guest during the vmlaunch/vmresume.

For a nested guest, during its vmcs programing from host VM, according
to above, its virtual vmcs (vmcs12) is saved in two places: vmcs02 for
shadow fields and cached_vmcs12 for no shadow fields. Meanwhile for
cached_vmcs12, there are also two parts for its fields: one is emulated
fields, the other one is host state fields. The emulated fields shall be
emulated to the physical value then fill into vmcs02 before vmcs02 active
to do vmlaunch/vmresume for the nested guest. The host state fields are
guest state of host vcpu, it shall be restored to guest state of host
vcpu vmcs (vmcs01) before return to host VM.

Below is a summary for contents of different vmcs fields in each above
mentioned vmcs:

               host state      guest state          control
 ---------------------------------------------------------------
 vmcs12*:       host VM	      nested guest         host VM
 vmcs02*:        pKVM         nested guest      host VM + pKVM*
 vmcs01*:        pKVM            host VM             pKVM

 [*]vmcs12: virtual vmcs of a nested guest
 [*]vmcs02: vmcs of a nested guest
 [*]vmcs01: vmcs of host VM
 [*]the security related control fields of vmcs02 is controlled by pKVM
  (e.g., EPT_POINTER)

Below show the vmcs emulation method for different vmcs fields for a
nested guest:

                host state      guest state         control
 ---------------------------------------------------------------
 virutal vmcs:  cached_vmcs12*     vmcs02*          emulated*

 [*]cached_vmcs12: vmexit then get value from cached_vmcs12
 [*]vmcs02:        no-vmexit and directly shadow from vmcs02
 [*]emulated:      vmexit then do the emulation

The vmcs02 & cached_vmcs12 is sync back to vmcs12 during VMCLEAR
emulation, and updated from vmcs12 when emulating VMPTRLD. And before
the nested guest vmentry(vmlaunch/vmresume emulation), the vmcs02 is
further sync dirty fields (caused by vmwrite) from cached_vmcs12 and
update emulated fields through emulation.


EPT Emulation (Shadow EPT)
==========================

Host VM launches its guest, and manage such guest's memory through a EPT page table
maintained in host KVM. But this EPT page table is untrusted to pKVM, so pKVM shall
not directly use this EPT as guest's active EPT. To ensure isolating of guest memory
for protected VM, pKVM hypervisor shadows such guest's EPT in host KVM, to build out
active EPT page table after necessary check (the check is based on page state
management which will be introduced later). It's actually an emulation for guest EPT
page table, the guest EPT page table in host KVM is called "virtual EPT", while the
active EPT page table in pKVM is called "shadow EPT".

How Shadow EPT be built?
------------------------

In native world, the guest EPT is majorly populated during guest EPT_VIOLATION VMExit
handling:

 1. guest access memory page which doesn't have a map in guest EPT, trigger
    EPT_VIOLATION;
 2. KVM MMU handle page fault for EPT_VIOLATION, allocate page then create corresponding
    EPT mapping.

For pKVM, the majority of guest EPT population is still same as native, but added more
steps for the shadowing:

 1. guest access memory page which doesn't have a map in shadow EPT, trigger
    EPT_VIOLATION;
 2. pKVM check if there is mapping in virtual EPT:
     - if yes, goto 5;
     - if no, goto 3;
 3. pKVM forward EPT_VIOLATION to host VM;
 4. KVM MMU in host handle page fault for EPT_VIOLATION, allocate page then create
    corresponding virtual EPT mapping, then VMResume back to guest, back to 1;
 5. pKVM shadow the mapping from virtual EPT to shadow EPT after page state check.

Emulate INVEPT
--------------

The simplest way to emulate INVEPT is to remove all mapping in shadow EPT, it leads to
EPT_VIOLATION for all gpa, then all mapping in shadow EPT will be re-created based on
updated virtual EPT. This will cause a lot of unnecessary shadow EPT_VIOLATION as most
of entries in virtual EPT is not changed. Optimized way is adding PV method to do INVEPT
with specific range, then shadow EPT only need removing mapping of necessary range every
time.


Memory Protection based on Page State
=====================================

To protect guest memory, pKVM introduces a mechanism called page state management. pKVM
uses two ignored bits in EPT page table entry (BIT 56 & 57) to track page states. At the
same time, when the page is un-present in EPT page table entry, ignored bits(12~31) may
be used to record page owner id.

 63 ... 58 |   57  56   |    ...    |  31 ... 12  | 11 ... 0
 ---------------------------------------------------------
 |  ...    | page state |    ...    |  [owner_id] |    ...


Page state - bits[57,56]:

  - PKVM_NOPAGE(00b):
	the page has no mapping in page table.
	under this page state, host EPT is using the pte ignored
	bits[31~12] to record owner_id.
  - PKVM_PAGE_OWNED(01b):
	the page is owned exclusively by the page-table owner.
  - PKVM_PAGE_SHARED_OWNED(10b):
	the page is owned by the page-table owner, but is shared
	with another.
  - PKVM_PAGE_SHARED_BORROWED(11b):
	the page is shared with, but not owned by the page-table
	owner.

Owner_id - bits[31~12] (only valid in host EPT when PKVM_NOPAGE):
  - 0: 	    PKVM_ID_HYP
  - 1:      PKVM_ID_HOST
  - others: PKVM_ID_GUEST

The page states can be recorded in different entities:
  - host EPT (with identical mapping)
  - guest shadow EPT (with mapping of gpa to hpa)

After a page is donated from host VM, the donee's id (page's owner id) can be recorded
in host EPT's corresponding page table entry.

Based on these, it's easy to find out a physical page's current owner and state.

Page state transition
---------------------

Below state machine defines how page states are transformed among different entities
(host EPT, and guest shadow EPT - which include normal VM & protected VM):


       +------------------+                   +------------------+
       |  host : NOPAGE   | <---------------- |  host : OWNED    |
       |  guestA: OWNED   | ----------------> |  guest: NOPAGE   |
       +------------------+       /           +------------------+
             |        ^          /                 |        ^
             |        |         /                  |        |
             |        |        /                   |        |
             |        |       /                    |        |
             |        |      /                     |        |
             v        |     /                      v        |
    +----------------------------+         +----------------------------+
    |   host : SHARED_BORROWED   |         |   host : SHARED_OWNED      |
    |   guestA: SHARED_OWNED     |         |   guestB: SHARED_BORROWED  |
    +----------------------------+         +----------------------------+


Initially, all pages except pKVM owned ones are owned by host VM, so these pages are
marked with PKVM_PAGE_OWNED in host EPT. Meantime, before guest first EPT_VIOLATION,
there is no page mapped in guest shadow EPT, so all page states in its shadow EPT are
PKVM_NOPAGE.

When guest EPT_VIOLATION happen, pKVM needs to do EPT shadowing to build shadow EPT page
mapping based on virtual EPT. During it, the corresponding page's state shall follow
above state machine to do page donation or page sharing.

1) page donation
-----------------

For a protected VM (guestA), during EPT shadowing, the page assigned to guestA shall be
donated from host VM. Which means the page's ownership is moved from host to guestA. So
in host EPT, the mapping of corresponding page table entry (host_gpa to hpa(== host_gpa))
is removed and its page state is marked as PKVM_NOPAGE (meantime the guestA is recorded
as owner_id). Meanwhile in guestA shadow EPT, the mapping of corresponding page table
entry (gpa to hpa) is setup and its page state is marked as PKVM_PAGE_OWNED.

Once a page is donated to a guest, it cannot be donated or shared to other guests before
undonate back to host.

Sometimes, host also need donate pages to the pKVM hypervisor (e.g., when creating a VM,
its shadow VM data strtucture is allocated in host then donated to the pKVM hypervisor).

2) page sharing
---------------

For a normal VM (guestB), during EPT shadowing, the page assigned to guestB shall be
shared from host VM. Which means both host VM and guestB can access this page. So
in host EPT, the mapping of corresponding page table entry is kept and its page state
is marked as PKVM_PAGE_SHARED_OWNED. Meanwhile in guestB shadow EPT, the mapping of
corresponding page table entry is setup and its page state is marked as
PKVM_PAGE_SHARED_BORROWED.

Once a page is shared to a guest, it cannot be donated or shared to other guests before
unshare back to host.

For a protected VM (guestA), a page can be shared back to host VM after donated to this
guest (e.g., to support virtio). For this case, in host EPT, the mapping of corresponding
page table entry is setup again and its page state is marked as PKVM_PAGE_SHARED_BORROWED.
Meanwhile in guestA shadow EPT, the mapping of corresponding page table entry is kept but
its page state is changed to PKVM_PAGE_SHARED_OWNED.

Once a page is shared back to host after donated, guestA is allowed to unshare it. And
this page can also be returned back to host directly.


Misc
====


NMI handling in pKVM
---------------------

Normally pKVM shall not trigger any exception, but NMI is not able to mask in vmx
root mode thus pKVM shall provide appropriate handler for it. Such NMI handler needs
to ensure NMI happened in vmx root mode is captured then injected back to host VM, to
avoid NMI lost.

The NMI injection is done as the last step before VMEnter to host VM, but there is
still case that a NMI happened just after the injection step. To avoid big delay, pKVM
enables irq window whenever there is a NMI happened in vmx root mode, then NMI injection
flow could be quickly done in next VMEnter. After it, host VM will VMExit once it open
interrupt, no matter the NMI is already injected or not. This may cause a dummy VMExit
but not cause any trouble.


[1]: https://lwn.net/Articles/836693/
[2]: https://lwn.net/Articles/837552/
[3]: https://lwn.net/Articles/895790/
[4]: https://kvmforum2020.sched.com/event/eE24/virtualization-for-the-masses-exposing-kvm-on-android-will-deacon-google
[5]: https://software.intel.com/content/www/us/en/develop/articles/intel-trust-domain-extensions.html
[6]: https://lore.kernel.org/linux-arm-kernel/20230201125328.2186498-1-jean-philippe@linaro.org/T/
[7]: https://kvmforum2022.sched.com/event/15jKc/supporting-tee-on-x86-client-platforms-with-pkvm-jason-chen-intel
[8]: https://lore.kernel.org/r/20210319100146.1149909-13-qperret@google.com
[9]: SDM: Virtual Machine Control Structures chapter, VMCS TYPES.
