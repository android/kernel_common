.. SPDX-License-Identifier: GPL-2.0

==============
KVM pvIOMMU
==============

pvIOMMU is a paravirtual IOMMU interface exposed to guests.

This is mainly needed for protected virtual machines (pVM), with device
assignment, as the host can't map memory for the guest kernel in the IOMMU
(as it is not trusted).

So the host kernel would create a pvIOMMU device and attach it to a guest VM
while providing a virtual (pvIOMMU, virtual SID(vsid)) mapping to
the physical device (IOMMU, SID) to the hypervisor to translate guest requests.

The interface as described below, mainly follows the Linux IOMMU ops
({attach, detach}_dev, {alloc, free}_domain, {map, unmap}_pages) which
makes the guest driver trivial to implement.

pvIOMMU ID, is chosen by the host and described to the guest in a platform
specific way, in pKVM reference implementation this is done through the device
tree.

The pvIOMMU exposes on hypercall ``ARM_SMCCC_VENDOR_HYP_KVM_PVIOMMU_OP_FUNC_ID``
where arg1 defines the operation requested as follows:


``KVM_PVIOMMU_OP_ATTACH_DEV``
--------------------------------------

Attach a device to a domain, previously allocated from ``KVM_PVIOMMU_OP_ALLOC_DOMAIN``

+---------------------+-------------------------------------------------------------+
| Presence:           | Optional; pKVM protected guests only                        |
+---------------------+-------------------------------------------------------------+
| Calling convention: | HVC64                                                       |
+---------------------+----------+--------------------------------------------------+
| Function ID:        | (uint32) | 0xC6000003E                                      |
+---------------------+----------+----+---------------------------------------------+
| Arguments:          | (uint64) | R1 | 0x0                                         |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R2 | pvIOMMU ID                                  |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R3 | vSID (virtual SID)                          |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R4 | PASID (or 0)                                |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R5 | Domain ID                                   |
|                     +----------+----+---------------------------------------------+
|                     | (uint32) | R6 | PASID_bits, the pasid space.                |
+---------------------+----------+----+---------------------------------------------+
| Return Values:      | (int64)  | R0 | ``SUCCESS (0)``                             |
|                     |          |    +---------------------------------------------+
|                     |          |    | ``INVALID_PARAMETER (-3)``                  |
+---------------------+----------+----+---------------------------------------------+

``KVM_PVIOMMU_OP_DETACH_DEV``
--------------------------------------

Detach a device from a domain, previously attached with ``KVM_PVIOMMU_OP_ATTACH_DEV``

+---------------------+-------------------------------------------------------------+
| Presence:           | Optional; pKVM protected guests only                        |
+---------------------+-------------------------------------------------------------+
| Calling convention: | HVC64                                                       |
+---------------------+----------+--------------------------------------------------+
| Function ID:        | (uint32) | 0xC6000003E                                      |
+---------------------+----------+----+---------------------------------------------+
| Arguments:          | (uint64) | R1 | 0x1                                         |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R2 | pvIOMMU ID                                  |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R3 | vSID (virtual SID)                          |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R4 | PASID (or 0)                                |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R5 | Domain ID                                   |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R6 | Reserved / Must be zero                     |
+---------------------+----------+----+---------------------------------------------+
| Return Values:      | (int64)  | R0 | ``SUCCESS (0)``                             |
|                     |          |    +---------------------------------------------+
|                     |          |    | ``INVALID_PARAMETER (-3)``                  |
+---------------------+----------+----+---------------------------------------------+

``KVM_PVIOMMU_OP_ALLOC_DOMAIN``
--------------------------------------

Allocate a translation regime, which can hold IOMMU mappings and used (attached)
by one or more devices.

After a domain is successfully created from this operation, it can be passed to
``KVM_PVIOMMU_OP_MAP_PAGES`` and ``KVM_PVIOMMU_OP_UNMAP_PAGES`` ops to add IOVA to
IPA mappins inside yet.
And can be used in ``KVM_PVIOMMU_OP_ATTACH_DEV`` and ``KVM_PVIOMMU_OP_DETACH_DEV``
to attach/detach devices to this translation regime.

+---------------------+-------------------------------------------------------------+
| Presence:           | Optional; pKVM protected guests only                        |
+---------------------+-------------------------------------------------------------+
| Calling convention: | HVC64                                                       |
+---------------------+----------+--------------------------------------------------+
| Function ID:        | (uint32) | 0xC6000003E                                      |
+---------------------+----------+----+---------------------------------------------+
| Arguments:          | (uint64) | R1 | 0x2                                         |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R2 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R3 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R4 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R5 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R6 | Reserved / Must be zero                     |
+---------------------+----------+----+---------------------------------------------+
| Return Values:      | (int64)  | R0 | ``SUCCESS (0)``                             |
|                     |          |    +---------------------------------------------+
|                     |          |    | ``INVALID_PARAMETER (-3)``                  |
|                     +----------+----+---------------------------------------------+
|                     | (uint32) | R1 | Domain ID in case of ``SUCCESS``            |
+---------------------+----------+----+---------------------------------------------+

``KVM_PVIOMMU_OP_FREE_DOMAIN``
--------------------------------------

Free a domain, previously allocated from ``KVM_PVIOMMU_OP_ALLOC_DOMAIN``
All device previously attached to this domain (``KVM_PVIOMMU_OP_ATTACH_DEV``)
MUST be detached first (``KVM_PVIOMMU_OP_DETACH_DEV``) before calling this.

+---------------------+-------------------------------------------------------------+
| Presence:           | Optional; pKVM protected guests only                        |
+---------------------+-------------------------------------------------------------+
| Calling convention: | HVC64                                                       |
+---------------------+----------+--------------------------------------------------+
| Function ID:        | (uint32) | 0xC6000003E                                      |
+---------------------+----------+----+---------------------------------------------+
| Arguments:          | (uint64) | R1 | 0x3                                         |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R2 | Domain ID                                   |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R3 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R4 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R5 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R6 | Reserved / Must be zero                     |
+---------------------+----------+----+---------------------------------------------+
| Return Values:      | (int64)  | R0 | ``SUCCESS (0)``                             |
|                     |          |    +---------------------------------------------+
|                     |          |    | ``INVALID_PARAMETER (-3)``                  |
+---------------------+----------+----+---------------------------------------------+

``KVM_PVIOMMU_OP_MAP_PAGES``
--------------------------------------

Map pages in a domain.

IOVA and IPA and size must all be aligned to memory protection granule according to
``ARM_SMCCC_KVM_FUNC_HYP_MEMINFO``

prot(R6) encoded as a bitmask as follows:
- ARM_SMCCC_KVM_PVIOMMU_READ		(1 << 0)
- ARM_SMCCC_KVM_PVIOMMU_WRITE		(1 << 1)
- ARM_SMCCC_KVM_PVIOMMU_CACHE		(1 << 2)
- ARM_SMCCC_KVM_PVIOMMU_NOEXEC		(1 << 3)
- ARM_SMCCC_KVM_PVIOMMU_MMIO		(1 << 4)
- ARM_SMCCC_KVM_PVIOMMU_PRIV		(1 << 5)

+---------------------+-------------------------------------------------------------+
| Presence:           | Optional.                                                   |
+---------------------+-------------------------------------------------------------+
| Calling convention: | HVC64                                                       |
+---------------------+----------+--------------------------------------------------+
| Function ID:        | (uint32) | 0xC6000003E                                      |
+---------------------+----------+----+---------------------------------------------+
| Arguments:          | (uint64) | R1 | 0x4                                         |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R2 | Domain ID                                   |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R3 | IOVA                                        |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R4 | IPA                                         |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R5 | Size                                        |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R6 | Protection                                  |
+---------------------+----------+----+---------------------------------------------+
| Return Values:      | (int64)  | R0 | ``SUCCESS (0)``                             |
|                     |          |    +---------------------------------------------+
|                     |          |    | ``INVALID_PARAMETER (-3)``                  |
+---------------------+----------+----+---------------------------------------------+
|                     | (uint64) | R1 | Number of mapped pages                      |
+---------------------+----------+----+---------------------------------------------+

``KVM_PVIOMMU_OP_UNMAP_PAGES``
--------------------------------------

Unmap pages from a domain.

+---------------------+-------------------------------------------------------------+
| Presence:           | Optional.                                                   |
+---------------------+-------------------------------------------------------------+
| Calling convention: | HVC64                                                       |
+---------------------+----------+--------------------------------------------------+
| Function ID:        | (uint32) | 0xC6000003E                                      |
+---------------------+----------+----+---------------------------------------------+
| Arguments:          | (uint64) | R1 | 0x5                                         |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R2 | Domain ID                                   |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R3 | IOVA                                        |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R4 | Size                                        |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R5 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R6 | Reserved / Must be zero                     |
+---------------------+----------+----+---------------------------------------------+
| Return Values:      | (int64)  | R0 | ``SUCCESS (0)``                             |
|                     |          |    +---------------------------------------------+
|                     |          |    | ``INVALID_PARAMETER (-3)``                  |
+---------------------+----------+----+---------------------------------------------+
|                     | (uint64) | R1 | Number of unmapped pages                    |
+---------------------+----------+----+---------------------------------------------+

``ARM_SMCCC_KVM_FUNC_DEV_REQ_DMA``
--------------------------------------

Verify a device IOMMU matches what the host describes in the firmware
(ex: device tree) for a physical device passthrough to a protected
virtual machine.

Called per IOMMU endpoint (pvIOMMU ID + vSID).

Returns a token(128 bit) that can be used to verify the resource
matching a trusted firmware description with the same token, which
passed is through a platform specific way.

Must be called before any IOMMU access for protected virtual machines.

Ideally called from protected vm firmware.

+---------------------+-------------------------------------------------------------+
| Presence:           | Optional.                                                   |
+---------------------+-------------------------------------------------------------+
| Calling convention: | HVC64                                                       |
+---------------------+----------+--------------------------------------------------+
| Function ID:        | (uint32) | 0xC6000003D                                      |
+---------------------+----------+----+---------------------------------------------+
| Arguments:          | (uint64) | R1 | pvIOMMU ID                                  |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R2 | vSID                                        |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R3 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R4 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R5 | Reserved / Must be zero                     |
|                     +----------+----+---------------------------------------------+
|                     | (uint64) | R6 | Reserved / Must be zero                     |
+---------------------+----------+----+---------------------------------------------+
| Return Values:      | (int64)  | R0 | ``SUCCESS (0)``                             |
|                     |          |    +---------------------------------------------+
|                     |          |    | ``INVALID_PARAMETER (-3)``                  |
+---------------------+----------+----+---------------------------------------------+
|                     | (uint64) | R1 | Token1 in case of ``SUCCESS``               |
+---------------------+----------+----+---------------------------------------------+
|                     | (uint64) | R2 | Token2 in case of ``SUCCESS``               |
+---------------------+----------+----+---------------------------------------------+
