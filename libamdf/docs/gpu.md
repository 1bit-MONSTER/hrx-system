# GPU user queues

libamdf creates native GPU queues and exposes the mapped state needed to
publish caller-prepared commands. The runtime owns executable loading, command
encoding, memory visibility, execution dependencies and resource lifetimes.
Those responsibilities remain the same whether the runtime uses individual
dispatches, recorded command buffers or persistent programs.

The [AMD GPU hardware reference](../../docs/reference/amd/gpu/README.md)
describes PM4, SDMA and AQL command semantics.
[GPU timing and performance counters](../../docs/reference/amd/gpu/observability.md)
describes native timestamp, clock and profiling mechanisms.

## Selection and preparation

`endpoint_query_queue_family_info` describes command representation separately
from publication. A runtime selects `GPU_AQL`, `GPU_PM4` or `GPU_SDMA`, a format
version, the required operations, and `USER` publication before creating the
device. The [GPU header](../include/amdf/gpu.h) specifies each format's packet
layout contract, index units and publication ordering. The selected family also
reports producer modes, priorities, ring limits and cache operations.

For AQL, the current Linux KFD provider materializes the queue descriptor and
per-XCC context storage for supported GFX9.4 targets. The caller selects that
service through family capabilities. A compiler still uses endpoint architecture
information to produce compatible kernel code; queue selection does not make
kernel code portable between architectures.

The caller creates code, kernarg, data and signal backing with the intended
device access. Code requires executable access. Device addresses come from each
memory attachment; a host mapping supplies a separate CPU view. The
[memory contract](memory.md) determines how host writes become visible and which
cache transitions the device requires.

`gpu->user_queue_create` takes the family ordinal, producer mode, priority,
optional ring size and optional fixed scratch. It prepares all native ring,
control, context-save and notification storage before returning. The caller
then uses `user_queue_map(queue, NULL, ...)` and
`user_queue_mapping_query_info` to obtain host ring, index and doorbell
addresses. These addresses remain stable for the mapping lifetime. Queue
creation belongs to device or execution-stream preparation; dispatch publication
reuses its established storage.

## AQL publication

AQL format 1 exposes native 64-byte packet slots with 64-bit packet indices.
Firmware owns the read index. The write index is a reservation frontier, so it
can include packets that another producer has reserved but not yet published.

For each packet, a producer:

1. Reserves an index. Multiple producers use atomic fetch-add on the write index.
2. Waits until the acquired read index permits reuse of the reserved ring slot.
3. Writes the packet body, including addresses and any native signal handles.
4. Release-stores the header/setup dword, making the complete packet valid.
5. Release-stores the packet index to the mapped 64-bit doorbell.

Single producers notify in increasing order. Multiple producers may finish
publication and notification out of order; an earlier unpublished slot blocks
later consumption. Firmware invalidates consumed slots before returning them
to the producer. Ring capacity limits unreclaimed packet storage, not the number
of workgroups or total execution time.

This path contains no libamdf submission call, scratch allocation or command
translation. The runtime owns reservation and publication synchronization.
Separate queues have independent progress; the runtime expresses dependencies
using the native mechanisms appropriate to its command model.

An AQL family advertising TRANSFER accepts the format-1
[confirmed PM4 transfer subset](../include/amdf/gpu.h). Its vendor packet borrows
an executable indirect buffer; the caller retains the complete immutable IB
through native execution completion. Ring consumption releases the packet slot
without authorizing IB reuse. The current KFD selector advertises this role on
gfx942; gfx940/941 retain their compute and cache-control roles.

## Fixed private memory

AQL kernels declare their private-segment requirement in compiler-produced
metadata. An all-zero scratch request admits only kernels with no private
segment. Otherwise the caller supplies `amdf_gpu_queue_scratch_t` with a
read/write device attachment and a fixed per-workitem capacity. That allocation
is exclusive to the queue through execution completion and destruction.

Retained scratch addresses physical wave slots. Its wave count is
`compute_unit_count * maximum_scratch_wave_count_per_compute_unit`, from the
public GPU endpoint information. Every slot receives
`round_up(maximum_private_segment_byte_length * 64, 1024)` bytes. The backing
starts at a 4096-byte-aligned GPU address and covers the full wave count.

For example, 68 private bytes per workitem need 5120 bytes per wave. A device
reporting 304 CUs and 32 scratch slots per CU therefore needs 49,807,360 bytes
for that queue. This is a capacity reservation, independent of how many
workgroups a particular dispatch launches. Extra backing does not increase the
declared capacity; each dispatch fits the configured private-segment limit.

Queue creation validates this geometry and programs its per-XCC descriptors.
Smaller scratch pools require a different firmware reclamation protocol and are
rejected by this format. There is no hidden scratch growth or reclaim handler.
The runtime can choose a queue capacity from its executable set before creating
the queue and reuse it across dispatches.

## Completion and release

AQL completion and barrier dependencies reference complete native 64-byte AMD
signal blocks. A signal is caller-owned device-visible memory with USER kind,
a signed 64-bit value at byte offset eight, and initialized unused fields. A
packet carries the block's GPU address. These are native packet operands;
libamdf does not create or retain a signal object for each dispatch.

A kernel dispatch decrements its completion signal once after all workgroups
finish. The runtime chooses fence scopes and observes completion with the
required acquire semantics before using results or releasing reachable storage.
Native polling signals do not provide a host event-loop notification by
themselves. The runtime's wait and notification strategy is a separate concern
from allocating the signal block.

There are distinct reuse boundaries:

| Observation | Storage it permits the caller to reuse |
| --- | --- |
| Acquired native read index, or successful `user_queue_wait_consumed` | Consumed packet slots in the command ring. |
| Native execution completion with the required visibility | Code, kernargs, data and completion storage whose final use has completed. |
| Successful `user_queue_destroy`, after execution completion and mapping release | The queue's borrowed scratch backing. |

`user_queue_query_status` samples progress and terminal state. The KFD provider
can enter the driver to inspect VM faults, and also observes firmware queue
errors. It is an explicit status operation, separate from mapped publication or
a direct signal load. A healthy sampled status is not execution completion.

Normal release stops producers, observes final execution completion and ring
consumption, releases the producer mapping, destroys the queue, and then releases
scratch and other remaining dispatch allocations. The caller checks every
release result. Failed native retirement follows the documented
[resource-release contract](memory.md#lifetime-and-failure), including
preservation of backing that may remain reachable. Instance native-lifetime
policy still controls which underlying KFD reclamation boundary is available;
it does not change the caller's public ownership obligations.
