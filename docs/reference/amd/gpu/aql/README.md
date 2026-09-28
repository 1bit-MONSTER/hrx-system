# AQL publication and execution

Architected Queuing Language (AQL) describes work in fixed-size packets
consumed by an HSA agent's packet processor. Kernel dispatch packets name
executable descriptors and arguments; barrier packets express dependencies and
memory fences. Packet publication, execution completion, and storage reuse are
separate events. HSA System Architecture 1.2 §§2.9–2.10 defines that
processing model. [Specification][hsa]

## Publication and ownership

A producer reserves a monotonically increasing packet index, waits until its
ring slot is available, and writes the packet body while its type remains
INVALID. It then publishes the first DWORD with release ordering and notifies
the doorbell. An index reservation alone does not publish a packet. ROCr's AMD
vendor-packet submission follows this body-before-header sequence and uses a
platform-specific store fence when the ring resides in device memory. [Header
definitions][header] · [Native publication caller][publication]

The [publication chapter](publication.md) details capacity and progress rules,
packet-index versus doorbell units, native queue mappings, and the distinct
host-store sequences used for system and device memory.

The read index permits reuse of consumed ring slots. Kernel arguments,
executable code, payloads, dependency signals, and indirect command buffers
can remain in use after their naming packet has been consumed. Their lifetime
ends at the completion of their last independent user. A completion signal is
atomically decremented by the packet processor; a dependent barrier continues
to borrow that signal until the barrier itself completes. These boundaries
follow HSA §§2.9.1–2, 2.9.6, 2.9.8–9 and 3.3.3.1. [Specification][hsa]

## Mechanisms

| Chapter | Native contract |
| --- | --- |
| [Publication and doorbells](publication.md) | Ring representation, reservation, atomic publication, notification, native mappings, and slot versus task ownership. |
| [Barriers and signals](barriers.md) | Header ordering, AGENT/SYSTEM fences, AND/OR dependencies, native signal storage, and AMD BARRIER_VALUE epochs. |
| [Kernel dispatch](dispatch.md) | Packet geometry, compiler descriptors, argument fetches, private/group resources, and executable publication. |
| [Carried memory operations](transfers.md) | AMD vendor-format-1 PM4 indirection, confirmed data movement, virtual-XCC routing, and command-storage lifetime. |
| [Profiling](profiling.md) | Dispatch timestamp capture, command-processor clocks, counter injection, result ownership, and profiling-tail completion. |

Standard AQL packets and AMD vendor packets have different specification
owners. For example, an ordinary kernel dispatch asks the packet processor to
interpret a descriptor and establish launch state. A vendor-format-1 packet
carries a PM4 indirect buffer; its existence does not define how arbitrary
shader state inside that buffer interacts with the surrounding AQL queue. The
[dispatch chapter](dispatch.md#pm4-shader-state-inside-an-aql-queue) describes
that distinction. [Standard packet types][header] · [AMD carrier
caller][publication]

Memory placement and fence scope are also distinct. A buffer in system memory
still needs a suitable native mapping, a producer/consumer dependency, and
visibility operations for the actual observers. The [architecture
overview](../architectures.md), [PM4 reference](../pm4/), [SDMA
reference](../sdma/), and [cross-engine recipes](../recipes/) supply the
adjacent contracts. The [source map](../../sources.md) records the primary
revisions used here.

[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[header]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2825-L2949
[publication]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1759
