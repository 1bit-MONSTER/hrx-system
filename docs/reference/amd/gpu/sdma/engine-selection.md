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
| `DmaPreferredEngine`: ISA major 12 with minor at least 5 | Return all available engines; the source treats them as equivalent instead of imposing the dedicated xGMI/host-facing split. |
| `DmaPreferredEngine`: ISA major 9 with minor 4 or 5, CPU/GPU transfer | Prefer engine 0 for host-to-device, and engine 1 plus engine 2 when more than two total engines exist for device-to-host. |

[Recommended-engine admission][register-peer] [Copy entry][copy-entry]
[Explicit-engine checks][on-engine] [Preferred-engine masks][preferred]

For distinct agents with peer SDMA enabled, the topology-based
`GetBlitObject(dst, src, size)` helper chooses the host-facing path for
CPU/GPU traffic, different or zero hive IDs, and
same-hive GPUs without an xGMI SDMA engine pool. Otherwise it assigns a peer to
an xGMI blit. Its CPU rule does not describe every path through
`DmaPreferredEngine`, whose target-specific choices appear above. Neither a
clear link flag nor one helper's fallback justifies bypassing the selected
entry point's engine restrictions. [Topology-based helper][topology-helper]

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

[allocation]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1811-L1919
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
