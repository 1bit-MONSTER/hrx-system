# Memory and execution recipes

A device handoff combines payload visibility, an execution dependency, and
resource ownership. The producer writes and releases the payload; a control
operation publishes progress; the consumer waits and acquires before reading.
Each independent user must finish before its storage can be reused. Packet
availability, address reachability, and physical memory placement supply
different parts of this contract.

The [CPU, GPU and NPU interop index](../../interop/README.md) connects these
GPU mechanisms to the host and array flows. [Pipeline composition](../../interop/pipelines.md)
adds returned credits, split/join ownership, progress, drain and measurement
across the complete graph.

| Recipe | Native mechanisms |
| --- | --- |
| [CPU and GPU memory handoff](host-device.md) | Mapping/cache properties, HSA release/acquire, native completion, CPU apertures and imported-buffer access. |
| [GPU and AI Engine shared-memory handoff](gpu-npu.md) | Per-device addresses, external DMA visibility, finite submission ordering, resident progress and final drain. |
| [External memory and synchronization](../../interop/external-memory.md) | Linux DMA-BUF and dependency objects, Vulkan imports and ownership transfers, Windows shared resources and fences. |
| [SDMA upload, AQL dispatch, SDMA download](#sdma-upload-aql-dispatch-and-sdma-download) | HSA dependency signals, packet fence scopes, SDMA completion and memory polling. |
| [Staged device-local memory](local-memory.md) | System-memory staging, GC9.4.3/4 HBM cache policy, shader/DMA visibility, and the host-aperture boundary. |
| [PM4 shader and SDMA handoffs](pm4-sdma.md) | RADV directional progress, shader cache transitions, native gang submission, and final-use joins. |

## SDMA upload, AQL dispatch and SDMA download

Consider input and output accessible to one logical GPU agent and its SDMA
engine, with CPU upload/readback storage and coherently mapped control
signals. The kernel code and descriptor have already been published for
executable use; kernargs and inputs are initialized before their consuming
dispatch. These are separate resources with separate visibility and lifetime
requirements. [Executable publication](../aql/dispatch.md)

ROCr's async-copy contract requires both agents to access both buffers and
system-level coherent payloads. Its documented general sequence has a sender
SYSTEM release before DMA and a receiving-device SYSTEM acquire before using
the result. DMA may sit outside the shader coherency domain. SYSTEM here is a
scope; the payload may reside in system memory or device-local HBM. [HSA copy
contract][copy-api]

The following composition uses those copy obligations and HSA packet ordering:

| Actor | Ordered work |
| --- | --- |
| CPU setup | Initialize signals and changing staging/kernarg bytes; perform the CPU mapping's required publication before making dependent device work visible. |
| Upload SDMA | Satisfy input dependencies and required cache operations; copy staging to input; publish upload completion. |
| AQL queue | BARRIER_AND on upload completion, followed by SYSTEM-acquire/SYSTEM-release kernel dispatch with its own completion signal. |
| Download SDMA | Wait for compute completion; apply the destination/source mapping's required cache operations; copy output to readback; publish transfer completion. |
| CPU | Acquire-observe final transfer completion, access readback, and retain all other resources until their independent final users have completed. |

The [staged local-memory flow](local-memory.md#actor-flow-and-ownership)
spells out this initial CPU publication and the separate code, control,
staging and device-local payload owners.

ROCr's blit kernel uses a NONE/NONE dependency barrier followed by a dispatch
with SYSTEM acquire and release scopes. The barrier supplies the dependency;
the dispatch supplies the payload transitions. A dispatch release completes
before its completion signal is decremented. Adding an unrelated earlier
acquire would not satisfy the acquire that must follow upload completion.
[Dependency builder][and] [Dispatch builder][dispatch] [HSA 1.2 §§2.9.1–2.9.2
and 3.3.8][hsa]

The SDMA part has a distinct native envelope. ROCr places dependency polls
before HDP/USER_GCR and the copy; it places cache release before completion.
The [cache chapter](../sdma/cache.md) identifies which template emits those
operations. Copying only a payload packet from that stream does not carry the
surrounding visibility contract with it. [SDMA submission][sdma]

### Control signals and reuse

AMD USER signals have a 64-bit value inside their native signal object. AQL
dependency packets refer to the signal object, while SDMA polls or updates its
value location. The [AQL barrier chapter](../aql/barriers.md) describes the
object interpretation; the [SDMA signal chapter](../sdma/atomics.md) describes
atomic decrement, non-atomic completion, and notification ownership.

A restricted one-writer protocol can keep a signal's high DWORD zero and
transition its value from one to zero. A classic SDMA DWORD fence/poll then
addresses only the low DWORD. This composition requires the high word to
remain zero, excludes concurrent writers, and keeps the terminal zero stable
through all waiters. It cannot be generalized into a 64-bit atomic timeline
merely by changing the stored values. ROCr's actual classic dependency path
samples a signal and may poll both words separately. [Dependency
construction][dependencies]

Reusing a signal requires its final waiter to finish, not merely its producer.
Reusing input/output storage requires the last shader or transfer read/write
to finish. Reusing primary-ring bytes requires the corresponding consumed
frontier. The final notification commands can also outlive the data-completion
update. [Signal and ring owners](../sdma/atomics.md#memory-and-lifetime)

### Progress constraints

The HSA async-copy contract excludes dependencies on future async-copy
submissions because their queue placement can deadlock. Native queue
scheduling and resource availability consequently belong to a composition's
progress argument; a memory poll alone does not guarantee that its producer
can execute. [Copy dependency restriction][copy-api]

## Extending an edge across devices

Crossing a device boundary preserves the same ownership chain, with additional
native mapping and coherency participants:

| Edge | Information needed by its native owners |
| --- | --- |
| CPU ↔ GPU | CPU mapping/cache attributes, GPU PTE policy, publication ordering, any host-aperture maintenance, and the completed device operation. |
| GPU queue ↔ GPU queue | Producer cache release, visible control storage, consumer acquire, and the last use on both queues. Two queues may share one device. |
| GPU ↔ peer GPU | Both GPU mappings, directed topology and cache routes, producer/consumer scopes, and each device's final-use completion. Atomic control protocols additionally require that operation and width on the participating route. |
| GPU ↔ AI Engine array | Shared backing and device addresses, external DMA visibility, shim/tile transfer completion, and the array program's own drain protocol. A shared CPU alias does not move bytes into or out of tile SRAM. |
| CPU upload ↔ executable use | Descriptor and code publication, instruction-fetch visibility, kernarg publication, and completion of the last executable user before replacement. |

The [device-local recipe](local-memory.md) explains concrete native mapping
predicates, and the [array execution reference](../../xdna/execution.md)
describes its independent controller and program lifetimes. Omitting a cache
operation under a coherent mapping leaves the execution and ownership edges
intact.

[copy-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2153
[and]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L682-L686
[dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L880-L911
[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[sdma]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L663
[dependencies]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L397-L442
