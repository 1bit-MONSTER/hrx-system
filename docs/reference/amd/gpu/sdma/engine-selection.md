# SDMA engine selection

An SDMA queue selects an execution engine before it consumes copy packets.
The packet's source and destination addresses do not select a different engine
or establish peer mappings. KFD distinguishes ordinary and xGMI-optimized
engine pools, while ROCr also uses directed topology recommendations and
transfer-specific restrictions. Engine selection, memory reach, cache scope,
and completion remain separate contracts. [KFD queue allocation][allocation]
[ROCr native queue selection][blit-create]

## Native engine identities

KFD reports `num_sdma_engines` and `num_sdma_xgmi_engines` separately. Let
`N` and `X` denote those counts for the selected native GPU node. Its engine-ID
space places ordinary engines in `[0, N)` and xGMI-optimized engines in
`[N, N + X)`. These are node-local engine IDs, not queue IDs, GFX target
numbers, or native SDMA IP revisions. [Queue allocation][allocation]

| `kfd_ioctl_create_queue_args.queue_type` | Value | Selection |
| --- | --- | --- |
| `KFD_IOC_QUEUE_TYPE_SDMA` | `0x1` | Allocate from the ordinary SDMA queue bitmap. The selected engine is `sdma_id % N`. |
| `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` | `0x3` | Allocate from the separate xGMI SDMA queue bitmap. The engine is `N + sdma_id % X`. |
| `KFD_IOC_QUEUE_TYPE_SDMA_BY_ENG_ID` | `0x4` | Use the requested zero-based `sdma_engine_id` and search for an available queue on that engine. KFD normalizes the queue to its ordinary or xGMI class. |

The explicit-engine path reports resource exhaustion when that engine has no
free queue; it does not silently choose another engine. ROCr's
`SupportsSdmaQueueByEngineId` requires KFD interface 1.17 or later. That
interface-version check belongs to the pinned runtime implementation, separate
from the native command format. [UAPI fields][uapi]
[Allocation and exhaustion][allocation] [ROCr version gate][version]

Engine indices and masks have different representations.
`recommended_sdma_engine_id_mask` is a bit set: bit `e` recommends native engine
ID `e`. For example, mask `0x4` names engine 2, not engine 4. ROCr's internal
`DmaCopyOnEngine` argument is instead a blit-table index; its table includes a
compute-copy entry. The source explicitly bounds that argument by the blit
table rather than treating it as an engine count. [Topology mask][recommendation]
[ROCr blit index][on-engine]

## Observing a live KFD queue

The pinned Linux implementation exposes read-only queue attributes below the
KFD device's sysfs directory. A primary context uses
`proc/<pid>/queues/<queue-id>/`; a secondary context uses
`proc/<pid>/context_<context-id>/queues/<queue-id>/`. The directory's queue ID
belongs to that KFD context, independently of the SDMA engine ID.
[Process roots][process-root] [Secondary contexts][process-context]
[Queue directory][queue-directory]

| Attribute | Native value |
| --- | --- |
| `type` | Decimal internal `kfd_queue_type`: ordinary SDMA is 1, xGMI SDMA is 3. |
| `gpuid` | Decimal native GPU-node ID owning the queue. |
| `size` | Decimal primary ring length in bytes. |

[Attribute readers][queue-attributes] [Attribute declarations][queue-files]
[Read-only mode][queue-file-mode]
[Internal type enum][queue-types] [Queue input translation][queue-input]

These values establish queue class and GPU ownership, not physical engine
affinity. Explicit-engine construction is normalized to ordinary or xGMI type
by the allocator, and the attributes expose neither `sdma_engine_id` nor the
original explicit-engine request. The internal type enum also differs from
the CREATE_QUEUE command-format input: PM4 and AQL compute requests both
become internal COMPUTE type 0, with their format stored separately. Sysfs
type 2 therefore does not identify an AQL queue.
[Engine normalization][allocation] [Type and format translation][queue-input]

A process can correlate one serialized queue creation with the added native
queue record, checking its GPU ID and ring length while keeping the queue
alive. Concurrent creation or destruction requires separate correlation; the
tree is not an atomic inventory snapshot. Metadata presence says nothing
about pending work, copy completion, payload visibility or safe storage reuse.

Queue-directory creation is best effort: the process queue manager calls
`kfd_procfs_add_queue` after successful native construction without propagating
that metadata operation's result. A missing directory consequently does not
prove that no native queue exists or that queue creation failed.
[Metadata publication][queue-metadata-publication]

## Directed topology information

KFD exports link records with `node_from` and `node_to`. The record describes
that direction; its recommendation need not match the reverse record. The
public definitions identify link type 11 as `CRAT_IOLINK_TYPE_XGMI` and expose
the following flag bits. [Topology export][export]
[Link definitions][link-definitions]

| Link flag | Bit | Meaning in the native definition |
| --- | --- | --- |
| `CRAT_IOLINK_FLAGS_ENABLED` | 0 | The link is enabled. |
| `CRAT_IOLINK_FLAGS_NON_COHERENT` | 1 | The link is non-coherent. |
| `CRAT_IOLINK_FLAGS_NO_ATOMICS_32_BIT` | 2 | The route excludes 32-bit atomics. |
| `CRAT_IOLINK_FLAGS_NO_ATOMICS_64_BIT` | 3 | The route excludes 64-bit atomics. |
| `CRAT_IOLINK_FLAGS_NO_PEER_TO_PEER_DMA` | 4 | The route excludes peer DMA. |
| `CRAT_IOLINK_FLAGS_BI_DIRECTIONAL` | 31 | The definition marks a bidirectional link. |

KFD's native link policy does not add the no-atomics bits for xGMI. It marks
PCIe GPU-to-GPU links non-coherent, and also marks xGMI links non-coherent
for its exact GC9.4.0 predicate. Those link properties do not allocate memory,
grant an agent access, choose PTE cache policy, or supply a shader/SDMA atomic
operation. The [peer memory flow](../recipes/host-device.md#peer-gpu-handoff)
requires those independent facts. [Native link policy][link-policy]

The thunk describes link latency in nanoseconds, bandwidth in MB/s, and
recommended transfer size in bytes. These are topology properties, not measured
application transfer rates or a promise that each route field is populated.
[Property units][link-units]

`kfd_set_recommended_sdma_engines` has a specialized eight-GPU topology path:
it requires a non-VF device, GPU peer, nonzero AID mask, one KFD node, at least
six xGMI SDMA engines, a discrete GPU, and eight physical xGMI nodes. It indexes
`rec_sdma_eng_map` using the source and destination physical socket IDs;
the six-engine variant shifts the table's engine ID right once. A recommendation
that lands in the ordinary-engine range is replaced with the xGMI engine mask.
Outside that predicate, the function recommends the xGMI engine pool for an
xGMI GPU peer when that pool exists, and the ordinary engine pool otherwise.
The table is native topology policy, not a portable fixed engine assignment
for a GFX target. [Recommendation construction][recommendation]

## ROCr copy executor and engine mask

The asynchronous `Runtime::CopyMemory` overload, `CopyMemoryOnEngine`,
`CopyMemoryStatus`, and `GetPreferredEngine` select the source GPU as the
executor when the source agent is a GPU, otherwise the destination agent.
Consequently, an ordinary GPU-to-GPU copy uses the source GPU's blit table
and native queues. `HSA_REV_COPY_DIR=1` reverses the agent arguments at the
public asynchronous-copy entries while leaving the address operands in place.
The engine-status and preferred-engine entries pass their agent arguments
through without that reversal. [Executor selection][executor]
[Copy entry points][copy-api-entry] [Engine query entry points][engine-query-entry]
[Direction flag][direction-flag]

`hsa_amd_memory_async_copy_on_engine` takes a one-bit HSA engine mask.
`CopyMemoryOnEngine` converts bit `e` to blit-table index `e + 1`; zero and
multi-bit masks are invalid. Index zero is the compute-copy entry. The table
index selects a runtime object, whose construction can change the native
engine request. [Mask conversion][executor] [Blit construction][blit-init]

For an SDMA-enabled base-profile agent, the lazy factory applies the following
native selection rules. [Factory admission][blit-init]

| Factory predicate in the pinned ROCr source | Native queue selection |
| --- | --- |
| Ordinary blit, ISA major 9 and minor at least 4 | Rotate the requested ordinary engine with `(rec_eng + 1) % NumSdmaEngines`. |
| KFD interface older than 1.17, or ordinary blit on ISA 9.0 with stepping below 10 | Remove the explicit engine request and use pool allocation. |
| No dedicated xGMI engines | Remove the explicit engine request and select the ordinary pool. |
| xGMI blit without admitted directed recommendations | `CreateBlitSdma` removes the explicit engine request and selects the xGMI pool. |
| SDMA initialization fails during a copy request | The lazy factory can construct a compute-copy implementation instead. |

[Factory predicates][blit-init] [xGMI request and initialization][blit-factory]
[Native queue construction][blit-create]

For example, with two ordinary engines, dedicated xGMI engines, ISA major 9
and minor at least 4, and KFD interface at least 1.17, a host-to-device copy
requesting HSA bit 0 reaches blit index 1. Its admitted SDMA factory requests
native engine 1. This follows from the factory's rotation; the requested HSA
bit alone is not proof of native engine identity. Likewise, the
`force_copy_on_sdma` parameter suppresses the
same-GPU compute-blit selection in `DmaCopyOnEngine`, but does not remove
the factory's later fallback. [Blit construction][blit-init]
[Explicit-engine selection][on-engine]

`hsa_amd_memory_copy_engine_status` reports currently free usable blit
entries, including whether an instantiated entry is SDMA and has pending
bytes. Its mask is transient availability rather than an immutable engine
capability or reservation. [Availability query][engine-availability]

## ROCr transfer policy

The pinned runtime has several selection entry points. Reading its
topology-based blit helper alone misses the explicit-engine checks and the
preferred-engine path used by `DmaCopy`.

| Entry point and predicate | Selected behavior |
| --- | --- |
| `RegisterRecSdmaEngIdMaskPeer`: KFD interface at least 1.17, ISA major 9 with minor at least 4, one-bit recommendation, and recommended-engine selection not disabled | Retain that peer's recommended mask. Otherwise store zero for that peer. |
| `DmaCopy`: preferred mask supplies an engine | Call `DmaCopyOnEngine` for that engine before the ordinary gang-copy path. The source identifies recommended-engine copies as gang factor one. |
| `DmaCopyOnEngine`: SDMA selected, distinct GPU agents in one nonzero hive, peer engines available, and dedicated xGMI engines present | Reject a host-facing blit index unless the runtime's recommended-engine override is active. Its comment attributes this restriction to the host-facing engines being unable to drive that xGMI path. |
| `DmaCopyOnEngine`: peer SDMA disabled for a peer copy, or SDMA globally disabled | Select the compute-copy blit. |
| `DmaCopyOnEngine`: SDMA selected, exact ISA 9.0.10 | Restrict use of the host-to-device blit for other non-local transfer directions; the source attributes the restriction to a RAS issue. |
| `DmaPreferredEngine`: ISA major 12 with minor at least 5 | Return a preference mask covering every engine bit, without querying whether it is busy; the source treats these engines as equivalent instead of imposing the dedicated xGMI/host-facing split. |
| `DmaPreferredEngine`: ISA major 9 with minor 4 or 5, CPU/GPU transfer | Return HSA bit 0 for host-to-device, and bit 1 plus bit 2 when more than two total engines exist for device-to-host. The factory then applies the native-engine mapping above. |

[Recommended-engine admission][register-peer] [Copy entry][copy-entry]
[Explicit-engine checks][on-engine] [Preferred-engine masks][preferred]

The `rec_sdma_eng_override_` condition comes from topology discovery. That
scan looks for a single-bit peer recommendation on a GPU with exactly six
xGMI engines, then propagates a discovered override to later GPU entries.
Before it finds one, encountering a GPU with any other xGMI-engine count
ends the scan. This is runtime topology policy, separate from native
BY_ENG_ID availability. [Topology override construction][engine-override]

For distinct agents with peer SDMA enabled, the topology-based
`GetBlitObject(dst, src, size)` helper chooses the host-facing path for
CPU/GPU traffic, different or zero hive IDs, and
same-hive GPUs without an xGMI SDMA engine pool. Otherwise it assigns a peer to
an xGMI blit. Its CPU rule does not describe every path through
`DmaPreferredEngine`, whose target-specific choices appear above. Neither a
clear link flag nor one helper's fallback justifies bypassing the selected
entry point's engine restrictions. [Topology-based helper][topology-helper]

## CLR peer-engine allocation

CLR `f9ba16bbe70e` uses ROCr's availability and preferred-engine queries in
`SdmaEngineAllocator::AllocateEngine`. It initially selects the device's
CPU read/write mask. For `SdmaP2P`, it accumulates each peer's observed free
engines with `peer_engine_mask_[peerAgent.handle] |= freeEngineMask` and
uses that union as the valid mask once it becomes nonzero. An engine observed
idle can therefore remain eligible when a later query reports it busy.
[Peer mask construction][clr-peer-mask]

The union is a history of observations, not a complete engine enumeration.
An engine that has never appeared free is absent from it; before any nonzero
peer observation, the initial CPU read/write mask remains in place. CLR
intersects availability and preference with its selected valid mask. For a
peer copy with a nonzero preferred mask, it then prefers those engines even
when another virtual queue has already been assigned one. This is a runtime
sharing policy, not an exclusive reservation or a guarantee of native
physical-engine identity. The ROCr blit-to-native mapping above still applies.
[Mask filtering and shared preference][clr-peer-selection]

## Copy flow and ownership

1. Select the source, destination, and executing GPU. Establish direct access
   to both allocations through their actual mapping/pool contracts.
2. Select the directed route and native engine class. Apply the relevant
   topology recommendation and runtime/native interface restrictions before
   constructing the queue.
3. Publish source data, establish the transfer dependency, and execute the
   mapping-specific cache transitions before the engine reads it.
4. Execute the copy and its release/completion sequence. The receiver observes
   completion and applies its acquire before consuming the destination.
5. Retain source, destination, dependency operands, queue control and command
   storage through their respective final readers. Queue consumption and
   data completion remain distinct retirement boundaries.

ROCr's asynchronous-copy API requires both named agents to access both buffers
at their current locations and requires system-coherent payloads. Its dependency
contract also excludes waiting on a future asynchronous-copy submission.
Engine choice does not relax these access, visibility, progress, or lifetime
requirements. [Copy API contract][copy-contract]
[Publication and retirement](publication.md)

The synchronous `hsa_memory_copy` implementation has a different ownership
contract. For allocations owned by different GPUs, its
`Runtime::CopyMemory(dst, src, size)` path stages through temporary system
memory, even when the devices are peers. The source explains that this call
cannot assume peer access remains granted for the copy's duration. Thus this
API's GPU-to-GPU result does not establish a direct peer route or provide the
same transfer path as an explicitly mapped asynchronous copy.
[Synchronous copy and temporary ownership][synchronous-copy]

[allocation]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1811-L1919
[clr-peer-mask]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L4225-L4264
[clr-peer-selection]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L4271-L4309
[uapi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L61-L98
[export]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L250-L279
[link-definitions]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_crat.h#L232-L255
[link-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L1210-L1258
[recommendation]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L1261-L1313
[link-units]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/hsakmt/hsakmttypes.h#L517-L535
[blit-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L222-L246
[version]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L906-L925
[register-peer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1203-L1220
[copy-entry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1233-L1250
[on-engine]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1345-L1407
[preferred]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1507-L1541
[topology-helper]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3505-L3599
[copy-contract]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2096-L2144
[executor]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L679-L745
[copy-api-entry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L435-L518
[engine-query-entry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L778-L802
[direction-flag]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L234-L235
[blit-init]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L974-L1062
[blit-factory]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L903
[engine-availability]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1429-L1505
[engine-override]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_topology.cpp#L465-L499
[synchronous-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L594-L677
[process-root]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L393-L408
[process-context]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L881-L909
[queue-directory]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L536-L558
[queue-attributes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L419-L434
[queue-files]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L486-L515
[queue-types]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L439-L446
[queue-input]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L281-L310
[queue-metadata-publication]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L492-L502
[queue-file-mode]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L53-L55
