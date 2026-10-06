# External memory and synchronization

External sharing lets different APIs or devices refer to the same backing
allocation. Each participant still has its own resource object, address
translation, cache path and execution owner. A successful import establishes
access under the importer's contract; the producer's release, the consumer's
wait and acquire, and the last user's completion establish the handoff.

This chapter covers linear compute buffers, Linux DMA-BUF, Vulkan external
objects and Windows D3D12 sharing. An NPU participant additionally needs a
native import and completion contract for that transport. An external-memory
extension or an operating-system handle does not supply that contract.

## Objects that travel together

| Object or property | Meaning |
| --- | --- |
| Shared backing | Storage kept alive by its native owner and the references the selected import retains. It may be larger than the logical tensor or buffer view. |
| Export handle | A typed reference or name accepted by a particular import API. Its ownership rules are separate from device execution. |
| Resource description | Byte extent, binding offset, layout, access and creation properties understood by both participants. |
| Device address | An address in one device/context mapping. A handle or another device's pointer is not an address translation. |
| Completion object | A fence, semaphore payload or native command result that identifies the relevant producer or final consumer. |
| Cache and ownership transition | The release/acquire operations that make the shared bytes accessible through the next participant's memory path. |

ROCr's portable DMA-BUF export illustrates the distinction between a range
and its containing object. It returns an FD and the byte offset of the
exporting pointer within that object. For range-relative byte offset `r`,
the exporter uses `pointer + r`; an importer mapping the whole object uses
`imported_base + export_offset + r`. The range, offset and accessible extent
travel with the handle. Fine-grained export does not guarantee fine-grained
consistency through another API. [ROCr export contract][rocr-export]

Vulkan similarly exports a memory object's payload, not a `VkBuffer` view.
Its buffer binding offset and logical extent remain application metadata.
A Vulkan-exported DMA-BUF can be larger than the requested allocation size.
Exporting a containing allocation does not encode a suballocation access
restriction. [Vulkan memory export][vk-memory-fd]

## Linux: backing references and dependency objects

The DMA-BUF exporter owns the backing and its mapping operations. An importer
obtains a reference, attaches the object to its device, and maps that
attachment for DMA. The mapping supplies addresses for that device; backing
can move under the exporter/importer protocol. After the final device user,
the importer unmaps, detaches and drops its reference.
[DMA-BUF device flow][dmabuf-device]

The cited `amdxdna` importer follows that flow: it takes a DMA-BUF reference,
attaches and maps it for bidirectional DMA, creates an imported GEM object
and shares the exporter's reservation object. Its destruction path reverses
the mapping and references. The native BO query supplies the NPU address;
neither the FD nor an exporting GPU virtual address substitutes for it.
[XDNA import][xdna-import] [Import destruction][xdna-free]
[Address interpretation](../gpu/recipes/gpu-npu.md#one-backing-allocation-several-address-spaces)

The synchronization objects have different payloads:

| Linux object | Payload and use |
| --- | --- |
| DMA-BUF FD | Shared backing, attachment operations and a reservation object. It is not a completion FD. |
| `sync_file` FD | A particular fence or fence collection. Export from a syncobj captures its current fence; later reset or replacement of the syncobj does not retarget that file. |
| DRM syncobj handle or syncobj FD | A persistent synchronization container. An exported syncobj FD and imported handles retain the same object. A timeline operation also names a 64-bit point; that point is not a byte offset or an FD. |

[Syncobj payload, sharing and snapshot rules][syncobj]

Implicit synchronization works when submission owners publish their accesses
as reservation fences and consult the appropriate predecessors. A reader
depends on preceding writers; a writer depends on preceding readers and
writers. Sharing the reservation object does not make a command parser
discover dependencies from opaque embedded addresses. The submission's
actual resource and dependency inputs decide which accesses participate.
[Reservation rules][dmabuf-resv]
[Opaque array commands](../xdna/execution.md#linux-context-and-command-representation)

`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` snapshots the reservation fences for the
requested access: READ waits for writers; WRITE waits for readers and
writers. A bridge can export those predecessors, submit explicit work that
waits for them, then import its completion using
`DMA_BUF_IOCTL_IMPORT_SYNC_FILE`. This three-step bridge is not atomic;
its owner serializes competing submissions and reservation updates across
the sequence. It is useful only when both native submission paths honor
the resulting dependencies. [Explicit/implicit fence bridge][dmabuf-sync-file]

### DRM handles and CPU mapping ownership

A DMA-BUF FD can travel between devices, while an imported BO handle belongs
to the importing native device. A CPU mapping through a DRM render FD also
needs that device's mapping offset. In ROCr `f9ba16bbe70e`, the KFD import
path obtains the BO and its `mmap_offset` through the same GPU device.
The thunk's `hsaKmtMemoryGetCpuAddr` returns the result of
`DRM_AMDGPU_GEM_MMAP`; despite its name, this is an offset for a subsequent
mapping, not a CPU pointer or a GPU virtual address.
[Import and offset query][rocr-native-import] [Thunk offset result][thunk-mmap-offset]

ROCr's host-backed virtual-memory allocation chooses the first enabled GPU
for both the GTT allocation and the shareable-handle construction, and
retains it as `drm_owner`. For an externally imported allocation without a
local region, CPU access similarly imports through an enabled GPU to obtain
the native handle and offset. CPU mapping then uses that owner's render FD;
native-handle destruction uses the same owner. An allocation in host memory
still has a GPU-side native owner for these DRM operations.
[Host allocation owner][rocr-host-owner] [Imported CPU mapping][rocr-cpu-import]
[Native-handle destruction][rocr-handle-release]

The public VMM import duplicates the incoming FD and defers per-GPU import
until access is enabled. A successful import therefore lets the caller close
its original FD without retiring the runtime's backing reference. That
reference does not reconstruct the exporter's pool metadata: pointer queries
for imported VMM allocations leave `agentOwner` and `global_flags` unset when
there is no local region. An importer's native device owner and an allocation's
original pool owner are different facts.
[FD ownership][rocr-vmm-import] [Imported pointer information][rocr-import-info]

### PCIe export admission

In the same ROCr revision, `VMemoryExportShareableHandle` rejects re-export
of imported VMM handles. With `HSA_AMD_DMABUF_MAPPING_TYPE_PCIE`, it requires
the DRM owner to be a GPU for which `is_xgmi_cpu_gpu()` or `LargeBarEnabled()`
is true. These are that runtime's export predicates. The function then uses
the ordinary DMA-BUF export call without passing the flag to the driver.
Success supplies an admitted handle; the importer's attachment, mapping,
placement and executing engine still determine the actual transfer route.
[VMM export and PCIe checks][rocr-vmm-export]

### Peer placement and exporter power

An AMDGPU DMA-BUF attachment's P2P eligibility affects both placement and
power ownership. In Linux `fe2ec83746e5`, an otherwise eligible attachment
calls `pm_runtime_get_if_active` on the exporter. An inactive exporter clears
`peer2peer`; attachment does not wake it to preserve P2P. An active exporter
stays referenced until detach. With runtime PM disabled, a balancing
no-resume reference supplies the same detach accounting. Attachment rollback
also releases the reference. [Attachment and detach][amdgpu-peer-power]
[Runtime PM reference semantics][runtime-pm-active]

The mapping path starts with GTT placement and includes VRAM only when the
BO prefers VRAM and that attachment remains P2P-capable. Its pin path also
checks the other attachments before allowing VRAM. Consequently a shared FD
and reachable PCIe topology do not promise retained VRAM placement. GC12+
DCC backing has a separate P2P exclusion because its compression metadata is
device-local. [Placement and mapping][amdgpu-peer-placement]
[Compression and route checks][amdgpu-peer-power]

KFD also has a path that shares the original BO for same-hive VRAM mappings
without creating a DMA-BUF attachment. Its separate DMA-BUF branch reaches
the exporter/importer protocol. The attachment power reference above belongs
to that protocol; it is not a universal extra reference taken for every
peer mapping. Neither path supplies a payload dependency or shader cache
transition merely by establishing access. [KFD attachment selection][kfd-peer-attachment]

### CPU access is another handoff

For CPU access through a DMA-BUF mmap, first join preceding device users,
then use `DMA_BUF_IOCTL_SYNC` START with the actual READ/WRITE direction.
Access the CPU mapping and finish with END before new device use. These
operations provide the cache-access boundary, not mutual exclusion. The
kernel implementation can wait for registered implicit fences, but cannot
wait for work absent from that dependency graph. Polling the FD observes
registered fences and does not replace cache preparation.
[CPU-access UAPI][dmabuf-cpu] [Kernel wait][dmabuf-begin]
[Poll semantics][dmabuf-poll]

A CPU mapping maintained through a different API follows that API's access
protocol. For example, the native XDNA BO synchronization performs explicit
CPU-cache maintenance; Vulkan mapped-memory flush/invalidate applies to its
mapped memory. Neither operation is an NPU output-DMA join or a GPU shader
completion by itself. [CPU/GPU access](../gpu/recipes/host-device.md#imported-buffers-and-final-use)
[Array mapping maintenance](../gpu/recipes/gpu-npu.md#visibility-at-the-device-boundary)

## Vulkan: choose the external contract at creation

Ordinary compute sharing uses a buffer whose intended storage/copy usages
and external handle type are known before creation. The relevant query is
`vkGetPhysicalDeviceExternalBufferProperties` with those actual buffer
flags, usages and handle type. `IMPORTABLE`, `EXPORTABLE`, compatible handle
types and any `DEDICATED_ONLY` requirement describe that combination; an
extension name alone is insufficient. Images have additional format,
tiling and layout contracts and are not interchangeable with this buffer
flow. [External buffer query][vk-buffer-query]
[External memory features][vk-memory-features]

The resource includes `VkExternalMemoryBufferCreateInfo`. Exportable
allocation includes `VkExportMemoryAllocateInfo`; import uses the matching
handle structure. Allocation and binding satisfy the buffer's size,
alignment and memory-type requirements, plus dedicated-allocation rules.
For a non-opaque imported FD, `vkGetMemoryFdPropertiesKHR` supplies eligible
memory types, intersected with the buffer's requirements. It is not a query
for `OPAQUE_FD`: Vulkan-origin opaque imports instead preserve the required
allocation size and memory type. [External buffer creation][vk-buffer-create]
[Allocation and import constraints][vk-allocation]
[FD memory properties][vk-fd-properties] [Binding requirements][vk-requirements]

Handle types also carry identity constraints. The specification's tables
require matching device and driver UUIDs for opaque FD/Win32 memory and
D3D12 heap/resource handle types. The DMA-BUF row imposes no UUID-match
restriction; actual import valid usage, FD properties and external-buffer
capabilities still apply. The separate `VkImportMemoryFdInfoKHR` valid-usage
text also requires the memory to originate on the same underlying physical
device. The unrestricted UUID row alone therefore cannot authorize an
arbitrary foreign-device import. The finite flow below exports GPU-owned
memory and returns to the original GPU memory object. A DMA-BUF, opaque FD
and D3D12 resource handle are different import types.
[Handle compatibility][vk-memory-identity] [FD import valid usage][vk-memory-fd]

On Linux, successful Vulkan memory import consumes its FD; failure leaves
ownership with the caller. Each export returns a newly owned FD, which the
application closes or transfers through a consuming import. RADV's ordinary
memory owner imports the BO and closes the input FD only after success.
The imported memory object retains its own payload reference.
[FD ownership][vk-memory-fd] [RADV import owner][radv-import]

### CPU-owned host-pointer imports

`VK_EXT_external_memory_host` supplies a different input: a host pointer,
rather than an FD. `HOST_ALLOCATION` names host-allocated memory;
`HOST_MAPPED_FOREIGN_MEMORY` names a host mapping of foreign memory. Neither
pointer retains its backing. `VkImportMemoryHostPointerInfoEXT` imports the
pointer through `VkMemoryAllocateInfo`; the allocation or foreign mapping
owner keeps the entire range valid and accessible until the imported memory
object is destroyed and all other users have finished.
[Host handle kinds][vk-host-kinds] [Host import ownership][vk-host-import]

The pointer is aligned to `minImportedHostPointerAlignment`;
`allocationSize` is an integer multiple of that alignment and fits within
the actual accessible allocation or foreign mapping. Query
`vkGetMemoryHostPointerPropertiesEXT` for the chosen pointer and handle type,
then select a memory type in both its `memoryTypeBits` and the buffer's
requirements. The buffer's external-handle capability and binding
size/alignment rules still apply. Pointer imports cannot specify a non-null
dedicated buffer or image. [Host allocation constraints][vk-host-allocation]
[Pointer query and extent][vk-host-import]

An ordinary CPU-accessible flow selects a compatible HOST_VISIBLE type.
The specification's separate non-host-visible import permits no application
access through the original pointer or its aliases while the imported
memory object exists, and leaves contents undefined on import and after
destruction. Platform restrictions can also reject an otherwise aligned
import. [Host import modes][vk-host-import]

The original host pointer is not a Vulkan mapping merely because it was
imported. Cache maintenance for access through that original pointer uses
the platform's synchronization primitives. Vulkan flush/invalidate applies
to accesses through the pointer returned by `vkMapMemory`. Execution
dependencies remain necessary on both paths. Registering the same backing
with an NPU additionally needs its native mapping/cache contract; dual
import does not itself establish coherence or release/acquire ordering.
[Original pointer versus Vulkan mapping][vk-host-original-pointer]

### Memory ownership and semaphore payloads

External ownership transfer is required even when a resource was created
with `VK_SHARING_MODE_CONCURRENT`. The exporting queue releases the affected
buffer range; the receiver waits for that release and acquires ownership.
On return, the external producer releases and the Vulkan queue acquires.
The Vulkan half uses a buffer memory barrier with the local family and the
applicable external-family sentinel. `VK_QUEUE_FAMILY_EXTERNAL` represents
the same underlying device and driver; `VK_QUEUE_FAMILY_FOREIGN_EXT` can
represent other drivers/devices, including non-Vulkan devices. FOREIGN
requires the applicable extension and sharing contract.
[External resource ownership][vk-external-ownership]
[EXTERNAL and FOREIGN][vk-external-families]

The dependency must order the ownership operations themselves, as well as
the compute accesses. Without the maintenance8 all-stages ownership-transfer
option, the specification uses ALL_COMMANDS for semaphore synchronization of
the release/acquire operations. Narrowing a wait to the eventual shader
stage does not automatically cover an earlier ownership acquire.
[Ownership transfer stages][vk-ownership-stages]

Memory export does not create a semaphore. External semaphore capability is
queried separately, including its handle type and binary/timeline form;
exportable types are declared at semaphore creation. Opaque FD semaphore
handles use reference transference. `SYNC_FD` uses copy transference and
temporary import into a binary semaphore; it represents one submitted
completion, not a reusable timeline handle. Successful FD semaphore import
consumes its FD. [Semaphore capabilities][vk-semaphore-properties]
[Binary/timeline query input][vk-semaphore-type]
[Semaphore creation][vk-semaphore-create]
[Import semantics][vk-semaphore-import]

For `SYNC_FD` export, no queue can be waiting on the source semaphore. Its
payload is signaled or has a pending signal, and the signal operation and
its prerequisite signal operations have been submitted. Copy-transference
export has the same effects on the source payload as a semaphore wait; if
it used a temporarily imported payload, its previous permanent payload is
restored. The exported completion and the source semaphore therefore have
distinct reuse rules. [Export preconditions][vk-semaphore-fd]
[Payload transference][vk-semaphore-transference]

Host-observed fences can relay execution between APIs when no compatible
device-side dependency import exists. The memory release/acquire still
occurs on the appropriate device paths. Likewise, HOST_COHERENT removes
Vulkan mapped-memory flush/invalidate calls, not device dependencies or
foreign-device visibility requirements. Noncoherent CPU readback follows
device release to host → fence signal → host wait → mapped-range invalidate.
[Host visibility](../gpu/recipes/host-device.md)
[Mapped-memory contract][vk-host-memory] [CPU readback chain][vk-host-read]

## Windows: shared objects, residency and fences

D3D12 shares heaps, committed resources and fences through typed NT handles.
`CreateSharedHandle` produces a handle; `OpenSharedHandle` creates the
corresponding interface on the receiving device. Sharing a committed
resource also shares its implicit heap and resource description. A placed
resource additionally needs its heap offset and matching description on
each device. The shared object does not imply equal GPU virtual addresses.
[D3D12 shared objects][d3d-shared] [Opening objects][d3d-open]

Same-adapter sharing and cross-adapter sharing have different
creation requirements. Shared heaps use `D3D12_HEAP_FLAG_SHARED`; the
documented shared-heap path excludes CPU-accessible UPLOAD/READBACK heaps.
A concrete cross-adapter buffer path uses a DEFAULT heap with SHARED and
SHARED_CROSS_ADAPTER, compatible heap flags, and placed buffers carrying
ALLOW_CROSS_ADAPTER. The shared backing is system memory, with the required
cross-adapter cache treatment supplied by the driver. This does not remove
application barriers or cross-adapter fence dependencies.
[Shared and cross-adapter heap rules][d3d-heaps]

A resource's state and its completion are separate. Compute UAV writes and
later shader/copy reads use the appropriate D3D12 state/UAV barriers. Across
queues, the producer enqueues a fence signal after its accesses and barriers;
the consumer queues a wait before its dependent work. `ID3D12Fence::Signal`
is an immediate CPU signal and does not mean that a preceding GPU dispatch
finished. A CPU relay uses `SetEventOnCompletion` and a host wait.
[Resource barriers][d3d-barriers] [Queue timelines][d3d-sync]
[Queue wait][d3d-wait]

Shared fences are created with their sharing flags; cross-adapter fences
have the corresponding cross-adapter flag. The fence value has one agreed
producer timeline and generation meaning. D3D12 permits rewinding a fence,
so unrelated signalers cannot safely treat it as an automatically monotonic
counter. `GetCompletedValue == UINT64_MAX` reports device removal, not a
successful payload. [Fence kinds][d3d-fence-flags]
[Fence value ownership][d3d-sync] [Device removal][d3d-fence-result]

Retained handles and interfaces do not replace residency. The physical
memory owner must be resident before access and remain so through the last
GPU user; an asynchronous `EnqueueMakeResident` has its own completion fence.
Eviction follows execution completion. An upload or readback staging resource
has its own copy completion and CPU mapping boundary. `Map` can perform
needed CPU-cache invalidation, but a persistent pointer still requires
explicit execution ownership and valid object lifetime.
[Residency contract][d3d-resident] [Asynchronous residency][d3d-resident-async]
[CPU mapping][d3d-map]

Vulkan/Windows interop uses the declared D3D12_HEAP, D3D12_RESOURCE or
D3D12_FENCE handle type and its compatibility queries, rather than treating
an arbitrary NT handle as opaque Vulkan memory. Successful Vulkan NT-handle
import retains the payload but does not consume the caller's handle;
`CloseHandle` remains the caller's obligation. KMT global-share handles have
a different lifetime: they do not retain the backing. [Windows import owner][vk-win32-import]
[Memory handle kinds][vk-win32-kinds] [Semaphore identity][vk-semaphore-identity]

MCDM defines Windows compute contexts, address spaces, scheduling and driver
completion. It does not define an AMD NPU import of a D3D12 heap, resource or
fence. That path additionally needs the NPU owner's accepted handle types,
allocation/cache restrictions, address mapping, dependency integration and
terminal-use contract. Linux DMA-BUF and a Windows MCDM context cannot supply
those missing Windows-specific facts.
[MCDM owner model][mcdm] [Native array execution](../xdna/execution.md)

## A finite GPU/API → array → GPU/API flow

One complete Linux composition starts with Vulkan-owned linear buffers and
a native array importer. It is conditional on successful, compatible imports
and on the array contract making output writes complete at the observation
point used by the GPU. It needs neither CPU payload copies nor a claim that
the two APIs accept each other's native fence objects.

1. Query and create the exportable buffers, memory and any external
   synchronization objects with the required properties. Import the backing
   into the array context. Retain the buffer descriptions, byte offsets,
   full mapping extents and independent per-device addresses. Establish any
   CPU initialization/cache boundary before device use.
2. Submit the GPU producer, followed by release of the shared ranges to the
   applicable foreign owner. Wait for the completion covering that release,
   either in the host or through a synchronization object that the array's
   submission owner explicitly accepts.
3. The native array submission acquires the external input path, transfers
   input to local storage, computes, and drains the required output DMA.
   Observe successful terminal status; join any continuing worker/channel
   user before reconfiguring or releasing its resources. Completion of an
   input read can retire that source borrow earlier, but does not retire
   output DMA or independent users of the allocation.
4. After array release/output completion, submit the Vulkan ownership
   acquire and payload visibility operations before the GPU consumer. Join
   its final use. A direct device-side wait is interchangeable with the
   host relay only when its native dependency contract orders that acquire.
5. For CPU output, perform the API's host release, completion wait and cache
   access sequence. Reuse each range only after every reader/writer of that
   generation has completed. Retire command storage and synchronization
   objects through their final fetch/wait users before releasing mappings,
   imported objects, memory objects and export handles.

[External release/wait/acquire][vk-external-ownership]
[Finite array ownership](../gpu/recipes/gpu-npu.md#a-finite-gpu--array--gpu-sequence)
[Memory-object final use][vk-free]

The reverse producer direction uses the same edges in reverse: completed
array output → Vulkan acquire → GPU work/release → array acquire. Handle
sharing removes a payload copy only when both mappings and their visibility
contract permit direct access. Otherwise, an explicit staged copy has its
own source-read and destination-write completion. A resident per-generation
flag protocol additionally needs the external ordering and atomic-observation
premises in the [GPU/NPU handoff](../gpu/recipes/gpu-npu.md#output-dma-and-a-ready-flag);
a finite host join does not establish them.

[rocr-export]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L4425-L4473
[rocr-native-import]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/driver/kfd/amd_kfd_driver.cpp#L594-L629
[thunk-mmap-offset]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/memory.c#L1263-L1289
[rocr-host-owner]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L4377-L4426
[rocr-cpu-import]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L4600-L4640
[rocr-handle-release]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L4721-L4733
[rocr-vmm-import]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L4962-L4975
[rocr-import-info]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L1109-L1167
[rocr-vmm-export]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L4930-L4959
[dmabuf-device]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/dma-buf/dma-buf.c#L660-L686
[amdgpu-peer-power]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c#L79-L152
[runtime-pm-active]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/base/power/runtime.c#L1225-L1265
[amdgpu-peer-placement]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c#L161-L241
[kfd-peer-attachment]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L917-L955
[xdna-import]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L999-L1043
[xdna-free]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L656-L663
[syncobj]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/drm_syncobj.c#L30-L194
[dmabuf-resv]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/linux/dma-buf.h#L373-L418
[dmabuf-sync-file]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/dma-buf.h#L94-L183
[dmabuf-cpu]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/dma-buf.h#L26-L86
[dmabuf-begin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/dma-buf/dma-buf.c#L1497-L1516
[dmabuf-poll]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/dma-buf/dma-buf.c#L279-L302
[vk-buffer-query]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/capabilities.adoc#L1383-L1468
[vk-memory-features]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/capabilities.adoc#L772-L829
[vk-buffer-create]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/resources.adoc#L1031-L1059
[vk-allocation]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L1571-L1661
[vk-fd-properties]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L2921-L2965
[vk-requirements]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/resources.adoc#L10501-L10518
[vk-memory-identity]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/capabilities.adoc#L670-L738
[vk-memory-fd]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L2773-L2919
[radv-import]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device_memory.c#L170-L182
[vk-host-kinds]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/capabilities.adoc#L613-L623
[vk-host-allocation]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L1674-L1698
[vk-host-import]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L2972-L3123
[vk-host-original-pointer]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L5310-L5328
[vk-external-ownership]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/resources.adoc#L12434-L12477
[vk-external-families]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/synchronization.adoc#L8048-L8086
[vk-ownership-stages]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/synchronization.adoc#L8199-L8219
[vk-semaphore-properties]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/capabilities.adoc#L1539-L1583
[vk-semaphore-type]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/xml/vk.xml#L4861-L4866
[vk-semaphore-create]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/synchronization.adoc#L3718-L3758
[vk-semaphore-fd]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/synchronization.adoc#L3925-L4026
[vk-semaphore-import]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/synchronization.adoc#L5130-L5223
[vk-semaphore-transference]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/synchronization.adoc#L4846-L4893
[vk-host-memory]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L5294-L5307
[vk-host-read]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L5414-L5456
[vk-win32-import]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L2466-L2480
[vk-win32-kinds]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/capabilities.adoc#L581-L611
[vk-semaphore-identity]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/capabilities.adoc#L1671-L1684
[vk-free]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/memory.adoc#L4938-L4969
[d3d-shared]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle
[d3d-open]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandle
[d3d-heaps]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps
[d3d-barriers]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
[d3d-sync]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization
[d3d-wait]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait
[d3d-fence-flags]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_fence_flags
[d3d-fence-result]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
[d3d-resident]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-makeresident
[d3d-resident-async]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-enqueuemakeresident
[d3d-map]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map
[mcdm]: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mcdm-architecture
