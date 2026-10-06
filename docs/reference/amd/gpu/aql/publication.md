# AQL packet publication and doorbells

An AQL producer places a complete packet in a shared ring and transfers its
ownership to the packet processor. Reservation chooses the packet's position;
publication makes its contents executable; the doorbell notifies the processor
that work is available. Consuming that packet releases a ring slot, while the
task it names can continue using arguments, code, signals and payload memory.
HSA System Architecture 1.2 §2.8 defines these separate transitions.
[Queue mechanics][hsa]

## Queue representation and index units

The HSA queue descriptor is read-only to submitting agents. Its ring is
writable under the reservation and ownership rules below. The standard layout
has the following byte offsets; the read and write indices are additional
queue properties accessed through HSA operations, not fields of this public
descriptor. [HSA §2.8, Table 2–1][hsa] [Runtime declaration][queue-layout]

| Byte offset | Field | Meaning |
| --- | --- | --- |
| 0 | `type`, 32 bits | MULTI = 0 or SINGLE = 1 in HSA 1.2. |
| 4 | `features`, 32 bits | Kernel-dispatch bit 0 and agent-dispatch bit 1. |
| 8 | `base_address`, 64-bit address | Base of the packet ring, aligned to 64 bytes. |
| 16 | `doorbell_signal`, 64-bit handle | Runtime-owned notification signal, not the MMIO address itself. |
| 24 | `size`, 32 bits | Capacity in **packets**, a power of two. |
| 28 | Reserved, 32 bits | Zero. |
| 32 | `id`, 64 bits | Queue identity unique over the application's lifetime. |

Every packet occupies 64 bytes. For capacity `N` and packet ID `P`, its slot
starts at `base_address + 64 * (P & (N - 1))`. Both indices start at zero:
`write_index` names the next reservation, and `read_index` names the oldest
slot not yet released by the packet processor. The specification describes
these 64-bit indices as never wrapping; only the ring address wraps. Masking
the address does not define a protocol for overflowing an index. Queue
creation can impose stronger size and alignment restrictions than the
standard's minimum. [HSA §§2.8.3–5][hsa] [Creation contract][queue-create]

AMD's firmware-facing `amd_queue_v2_t` adds `write_dispatch_id` and
`read_dispatch_id`, retaining the earlier AMD queue layout. In its 64-bit
layout these 64-bit fields are at byte offsets 56 and 128; the enclosing AMD
structure has 64-byte alignment. ROCr supplies their addresses to native queue
creation and initializes them before attaching the queue. These native fields
do not make direct index access part of the portable HSA descriptor contract.
[AMD queue layout][amd-queue] [Native construction][queue-construct]
[Native attachment][queue-attach]

The native KFD interface uses a byte length for ring creation, while these AQL
indices count packets. Its GFX9/GFX11 MQD paths explicitly distinguish AQL
packet counts from PM4 DWORD counts, including a four-bit shift when handing
an AQL write pointer to the hardware loader. The same builders select
slot-based write-pointer mode and queue-full handling; the GFX9 builder also
sets `WPP_CLAMP_EN`, which the GFX11 source identifies as removed in GC10.
These are native queue-context settings, not packet-header fields.
[Creation units][thunk-create] [GFX9 load][mqd9-load] [GFX11 load][mqd11-load]
[GFX9 context][mqd9] [GFX11 context][mqd11]

## Reservation, capacity and forward progress

Reserving one position increments `write_index`; the old value is its packet
ID. Multiple producers use an atomic read-modify-write operation. A sole
producer can maintain a private next index and publish the updated index with
an atomic store. A queue declared SINGLE additionally requires in-order packet
assignment and monotonically increasing doorbell values. MULTI allows
producers to finish their reserved packets out of order. Dispatch still stops
at an earlier INVALID packet. [HSA §§2.8.3–5][hsa]

A producer may modify reserved packet `P` only when `P < read_index + N` and
the slot is INVALID. The packet processor makes the INVALID format globally
visible before advancing the read index past that slot. An acquired read-index
observation can therefore establish reuse of the slot; a cached older read
index is conservative. It cannot establish completion of the packet's task.
[HSA §2.8.3][hsa] [ROCr acquired load][index-loads]

HSA describes two reservation strategies:

| Strategy | Capacity and progress consequence |
| --- | --- |
| Check space, then compare-and-swap the write index | A full queue can be reported before claiming a position. Competing producers retry the reservation. |
| Fetch-add first, then wait for space | The reservation may extend beyond currently writable storage. Its owner must wait before touching the slot and eventually publish the reserved position so later packets can dispatch. |

ROCr's ordinary blit producer uses the second strategy. For a batch of `n`
packets starting at `P`, it requires `n <= N` and waits until
`P + n - read_index <= N`, then notifies `P + n - 1`. The single-packet native
vendor producer uses the equivalent `P - read_index < N` condition.
[HSA §2.8.4][hsa] [Blit reservation and notification][blit-reserve]
[Native vendor reservation][vendor-reserve]

CLR's ordinary dispatch path instead passes `N - 1` to its slot-wait helper.
The helper checks a cached read index and refreshes it with an acquired HSA
load when space may be exhausted. That deliberately stricter capacity policy
does not reduce HSA's architectural capacity to `N - 1`. A batch crossing the
physical ring end still addresses each packet modulo `N`; ordinary wrap does
not make a linear copy beyond the allocation valid. [CLR dispatch][clr-submit]
[Cached space check][clr-space]

Increasing the write index is neither publication nor an execution dependency.
A producer stalled after reservation can hold up later valid packets. The
reservation strategy and the work needed to release a full queue must preserve
progress together; a later doorbell cannot fill an unpublished position.
[HSA §§2.8.3–4][hsa]

## Packet publication

INVALID is packet type 1, not an all-zero packet. HSA requires the first 32
packet bits to be accessed with 32-bit atomic transactions. This word contains
the 16-bit header and the packet-specific next 16 bits, such as dispatch
`setup`. The producer publishes their complete value together after the rest
of the packet is globally visible. ROCr's blit caller and CLR's header helper
use a 32-bit release store for this transition. [HSA §§2.8.3 and 2.9.5][hsa]
[Blit packet publication][blit-publish] [CLR header store][clr-header]

A normal host producer follows this sequence:

1. Prepare the native queue, mapped ring, indices and doorbell. Publish code,
   arguments and input data for their actual consumers; packet publication
   does not replace [executable publication](dispatch.md#executable-publication-and-final-use)
   or the required [fence scopes](barriers.md#fence-scope-and-observers).
2. Reserve a packet ID, then establish that its physical slot is writable.
3. Write the packet body while its format remains INVALID. Preserve the
   packet-specific reserved fields required by its ABI.
4. Complete the mapping-specific body-to-header ordering, then atomically
   release the first DWORD with the valid header.
5. Complete the required packet-to-doorbell ordering and notify that packet's
   ID, or the last valid packet ID of the submitted batch.
6. Observe task completion and visibility before consuming results or reusing
   task resources. Observe slot retirement separately before overwriting the
   ring position.

ROCr's vendor caller illustrates the split-store form directly: it copies
bytes 4–63 first, performs the selected device-memory ordering, stores the
first DWORD with release semantics and rings the doorbell. Its completion
wait is a separate operation. [Native publication caller][vendor-publish]

Ownership transfers when the format becomes valid. The packet processor may
then modify the packet and may execute it **before** notification. The doorbell
is therefore not a gate for holding already-valid work. Conversely, the
processor need not process work until notified. A producer cannot use either
early execution or a missing notification as its synchronization protocol.
[HSA §2.8.3][hsa]

## Doorbell values and native mappings

For a batch starting at `P` with `n` packets, the new write index is `P + n`,
but the doorbell value is the last notified packet ID, `P + n - 1`. It is not
a ring byte offset, DWORD offset, masked slot number or next unassigned ID.
The notified packet must already be valid and globally visible. On HSA's
small machine model the value uses the packet ID's low 32 bits; this is a
machine-model rule, separate from a GPU's MMIO access width.
[HSA §2.8.3][hsa] [Ordinary notifier][blit-reserve]

Producers write the doorbell; only the packet processor evaluates it. HSA
does not define producer readback as a completion observation and disallows
atomic read-modify-write operations on the doorbell signal. MULTI notification
values need not be globally monotonic; SINGLE values must be. The runtime owns
the signal and its lifetime. [HSA §§2.6 and 2.8.3–4][hsa]
[Doorbell handle contract][queue-layout]

The pinned ROCr GPU-agent constructor accepts only KFD DoorbellType 2. On its
ordinary native path, `AqlQueue::StoreRelease` applies a release fence and
`StoreRelaxed` executes x86 `SFENCE` before a 64-bit MMIO store of the packet
ID. The named relaxed signal operation therefore still contains this native
ordering instruction. The DTIF and DXG branches instead call the thunk's
queue-doorbell operation; they are different transports. [Agent predicate][agent-doorbell]
[Native doorbell store][doorbell-store]

Linux maps the KFD doorbell allocation noncached. The thunk obtains the
process/device mapping and queue offset from queue creation. For SOC15, the
returned offset includes the doorbell's position within that mapping; deriving
it from the queue ID would use the older convention. A doorbell mapping and
the queue's packet-buffer mapping have separate cache attributes and owners.
[Native mapping][doorbell-map] [Offset selection][thunk-doorbell]

| Native generation distinction | Source-selected behavior |
| --- | --- |
| Thunk `gfxv >= 0x90000` | Eight-byte doorbell entries. Older entries are four bytes. |
| Kaveri, Hawaii and Tonga | KFD advertises pre-1.0 doorbell type 0. |
| Carrizo, Fiji, Polaris10/11/12 and Vegam | KFD advertises doorbell type 1. |
| KFD's newer capability path | Advertises doorbell type 2, the only type admitted by this ROCr revision. |

[Entry widths][thunk-width] [Older type selection][topology-old]
[Newer type selection][topology-new] [Runtime admission][agent-doorbell]

The thunk's capability-structure comment still calls values 2 and 3 reserved;
the native UAPI and the actual driver/runtime paths explicitly define and use
type 2. The legacy fields retained in the AMD queue structure likewise do not
establish a legacy store algorithm in this ROCr path. Its direct 64-bit store
must not be projected onto the older four-byte interface.
[Older comment][thunk-capability] [Native definitions][doorbell-types]

## Ring placement and x86 stores

AMD's queue-creation extension selects packet-buffer and queue-descriptor
placement separately. The default is system memory. A device-memory packet
ring does not imply device-memory indices: the latter placement requires
CPU atomic access and is explicitly excluded for PCIe-connected devices by
the extension's descriptor contract. [Placement flags][placement]

ROCr's device-ring allocator requires Large BAR, requests executable access
for CP packet fetches and selects GPU-uncached backing. Its ordinary system
allocation also requests executable access. GPU UC is a PTE/cache-route
attribute, not a declaration that the CPU mapping is UC. CLR distinguishes
write-combining PCIe mappings from CPU write-back coherent mappings and uses
non-temporal packet stores on its x86 path. These stores still need their
specified ordering when the underlying mapping is write-back.
[Ring allocation][ring-allocation] [CLR mapping and store rationale][clr-x86]

The [KFD storage contract](../architectures.md#kfd-queue-storage) separately
requires BO mappings for the ring and control words. ROCr's later nonpaged
allocation policy preserves that backing when ordinary pageable host storage
can instead use SVM. Executable access, cache attributes and backing ownership
are independent allocation properties.

CLR requests a device ring only when its placement setting, Large BAR and
the queue-creation entry point permit it. On Linux/x86, its ordinary split-store
path copies the 60-byte body without touching the first DWORD, issues `SFENCE`,
then performs the release header store. The later ROCr doorbell call supplies
its own `SFENCE` before MMIO. ROCr's vendor and blit producers also insert a
body-to-header `SFENCE` when the ring is in device memory and their queue's
PCIe-ordering predicate is true. [Placement caller][clr-placement]
[Body copy and fence][clr-body] [Split publication][clr-publish]
[ROCr vendor publication][vendor-publish] [Blit publication][blit-publish]
[Queue ordering predicate][queue-construct]

CLR has another Linux/x86-64 path when the CPU's MOVDIR64B feature and runtime
setting are enabled and the ring is in device memory. It stages a complete
packet, merges the valid first DWORD and issues one 64-byte direct store to the
aligned ring slot. It relies on that whole-packet write instead of the separate
body/header publication described by the portable HSA rule. An earlier fence
orders kernargs and any metadata before the packet. The normal ROCr notifier
still fences; CLR's optional direct notifier omits its additional fence for
this MOVDIR64B path. That omission is this runtime's x86/UC-doorbell policy, not
a portable AQL rule or permission to omit ordering on another mapping or
transport.
[CPU selection][clr-cpu] [Queue selection][clr-queue-selection]
[Direct packet store][clr-direct-packet] [Publication caller][clr-publish]
[Direct notifier][clr-direct-doorbell] [Notifier selection][clr-notifier]

Newer queues can also have a metadata-prefetch ring. ROCr selects its first
metadata version only with native capability, an explicit request, and a
reported agent ISA whose major version is 12 and minor version is at least 5.
CLR's publication path orders that metadata before making the dispatch valid.
Ordinary 64-byte packet publication alone does not describe this extra owner.
[Metadata predicate][metadata-select] [Metadata ordering][clr-publish]

Older ring-mapping conditions are separately visible in KFD: topology marks
Tonga with `AQL_QUEUE_DOUBLE_MAP`, while the buffer-acquisition path handles
GFX7/GFX8 AQL rings whose reported size is twice their backing size. Those
legacy mapping conventions are not a general consequence of ring wrap and
are outside the Type-2-only ROCr construction above.
[Topology flag][double-map] [Native backing-size calculation][queue-buffers]

## Completion and queue lifetime

Three owners finish at different points:

| Storage | Reuse or release boundary |
| --- | --- |
| One ring slot | The processor invalidates it and advances the read index. |
| Arguments, executable code, payload and signals | Completion and visibility of their last independent users, including consumers on other queues. |
| Ring allocation, indices and doorbell | The queue owner stops publication and completes native queue teardown before releasing the underlying resources. |

AQL packets dispatch in order but can complete out of order. A last packet's
ordinary completion is not automatically a join of earlier independent tasks;
the [barrier and signal protocol](barriers.md) supplies those dependencies.
HSA queue destruction leaves unfinished packet results undefined, and queue
inactivation aborts pending execution. Neither operation is a substitute for
waiting for results that the program requires. [HSA §2.8.3][hsa]
[Destroy and inactivate contracts][queue-destroy]

The native resource owners preserve the distinction. KFD acquires references
to the ring and index backing during queue construction; its ordinary destroy
path invokes queue removal before releasing those buffer references. ROCr
stops queue handlers, inactivates the native queue, then frees ring memory.
This is the successful-removal ordering, not a promise that every teardown
failure proves completion. [KFD backing references][queue-buffers]
[KFD removal][queue-remove] [ROCr teardown][queue-teardown]
[Native inactivation][queue-inactivate]

On its normal waiting path, CLR's terminal memory fence emits a barrier when
needed, waits for its tracked completion and then resets reusable argument
storage. Its physical queue can remain in a runtime pool after a logical stream
releases its reference; returning a queue reference and destroying its ring are
different operations. The queue owner's lifetime does not extend arbitrary
payload lifetimes unless the application or runtime retains those owners too.
[Terminal fence and argument reuse][clr-retirement]
[Physical queue references][clr-queue-lifetime]

Return to [AQL](README.md), [kernel dispatch](dispatch.md), or the
[primary source map](../../sources.md).

[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[queue-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2319-L2382
[queue-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2384-L2405
[amd-queue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_queue.h#L60-L153
[queue-construct]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L81-L155
[queue-attach]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L268-L291
[thunk-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/queues.c#L761-L805
[mqd9-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L243-L255
[mqd11-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L193-L205
[mqd9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L260-L317
[mqd11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L214-L262
[index-loads]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L419-L443
[blit-reserve]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L868-L885
[vendor-reserve]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1618-L1631
[clr-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1524-L1574
[clr-space]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.hpp#L862-L877
[blit-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L892-L930
[clr-header]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1335-L1343
[vendor-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1729-L1757
[agent-doorbell]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L138-L166
[doorbell-store]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L482-L498
[doorbell-map]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_doorbell.c#L106-L145
[thunk-doorbell]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/queues.c#L820-L851
[thunk-width]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/queues.c#L39-L43
[topology-old]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L2142-L2169
[topology-new]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L1988-L1992
[thunk-capability]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/hsakmt/hsakmttypes.h#L199-L211
[doorbell-types]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_sysfs.h#L38-L44
[placement]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L3722-L3746
[ring-allocation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L594-L638
[clr-x86]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/utils/nontemporal.hpp#L29-L80
[clr-placement]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3512-L3531
[clr-body]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/utils/nontemporal.hpp#L334-L358
[clr-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1436-L1462
[clr-cpu]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocdevice.cpp#L1694-L1703
[clr-queue-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1262-L1289
[clr-direct-packet]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/utils/nontemporal.hpp#L384-L436
[clr-direct-doorbell]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/utils/nontemporal.hpp#L360-L375
[clr-notifier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1464-L1471
[metadata-select]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L119-L126
[double-map]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L512-L514
[queue-buffers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L196-L273
[queue-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2526-L2571
[queue-remove]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L557-L586
[queue-teardown]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L354-L398
[queue-inactivate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L737-L745
[clr-retirement]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2337-L2366
[clr-queue-lifetime]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3670-L3726
