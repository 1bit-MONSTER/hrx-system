# Heterogeneous pipelines

A heterogeneous pipeline composes memory handoffs into a graph of CPU work,
GPU shaders or transfers, and NPU dataflow. Each edge transfers a particular
version of data. Each node owns the reads and writes it initiates, including
DMA and work delegated to other processors. A node's published completion
can stand for a larger subgraph only when its dependency chain covers that
subgraph's relevant accesses.

The [directed handoff matrix](README.md#the-six-directed-handoffs) supplies the
individual edges. The sequences below compose those contracts; they do not
introduce another native queue format or assume a common device memory model.

## A finite CPU → GPU → NPU → GPU → CPU flow

Consider CPU input `A`, GPU-produced array input `B`, array-produced GPU input
`C`, and final output `D`. Each name denotes a logical range. Their native
allocations may be separate; each shared range has a mapping in every device
which accesses it. NPU-local buffers are additional storage behind the array's
DMA and stream graph. External allocation setup is described in
[external memory](external-memory.md).

| Phase | Execution and visibility | Storage still borrowed afterward |
| --- | --- | --- |
| Initialize | The CPU prepares code, arguments, control and `A`, then finishes the required host publication. | All storage reachable by accepted GPU/NPU work. |
| GPU producer | The GPU acquires `A`, computes `B`, joins its contributors and releases `B` to the NPU's external-memory path before signaling. | `B` remains owned by downstream NPU readers. `A` is reusable when its last reader has finished. |
| Array input and compute | After GPU completion, MM2S reads `B` into array-local storage. Local locks and channels transfer that storage through the workers. | `B` can retire after every external read; local objects remain live until their local consumers release them. |
| Array output | S2MM writes `C`; the finite controller joins the accesses covered by its result before successful terminal completion. | `C` remains borrowed by the GPU consumer. Resident local state, if present, retains its separate lifetime. |
| GPU consumer | After the NPU dependency, the GPU acquires `C`, computes `D`, and releases the output to its host-visible path before completion. | `C` retires after its last GPU or other reader. `D` remains live for CPU readback. |
| CPU result | The CPU observes successful completion and performs the readback mapping's acquire/cache-access operations. | `D` retires after the CPU and all other consumers finish. |

This is the composition of the complete
[CPU/GPU sequence](../gpu/recipes/host-device.md#a-complete-fine-grained-aql-flow),
[GPU/array sequence](../gpu/recipes/gpu-npu.md#a-finite-gpu--array--gpu-sequence),
and [CPU/array sequence](cpu-npu.md). A host wait can relay each dependency
while `B` and `C` stay in shared backing. A native inter-device fence path can
replace that relay when both submission owners implement its wait/signal and
visibility contract. Passing an allocation handle alone supplies no such
replacement.

If `D` resides in GPU-local memory, readback adds a transfer node and another
completion. The shader's last store releases neither the readback source nor
the CPU staging destination by itself. Likewise, a GPU-local `B` outside the
NPU's reachable mapping needs an explicit transfer to reachable backing.
[Staged local memory](../gpu/recipes/local-memory.md)
[Async-copy access and scope][async-copy]

## Resident execution and slot generations

A resident shader or array program spans several data versions in one native
invocation. Each version has a publication edge inside that invocation. A
queue-level acquire at entry does not acquire data written later, and a
dispatch-end release occurs too late to publish intermediate output.
[Resident GPU/array publication](../gpu/recipes/gpu-npu.md#resident-programs-and-per-generation-ownership)

A bounded single-producer/single-consumer channel illustrates the ownership
without requiring a particular control-cell encoding. For slot `s` and
generation `g`, the sequence is:

```text
producer owns slot
  -> writes payload(g)
  -> completes/releases payload to the consumer's path
  -> publishes ready(g)
consumer observes ready(g)
  -> acquires payload on its own path
  -> performs every read of payload(g)
  -> completes those reads and release-publishes returned credit(g)
producer acquire-observes returned credit(g)
  -> may write the next generation of this slot
```

This is a composition of the directed release/wait/acquire contracts, not an
atomicity guarantee for an arbitrary flag representation. Both `ready` and
returned credit need an observable control path, an admitted access width and
ordering with the accesses they publish. Payload fields become immutable
between ready publication and credit return. A producer cannot reuse the slot
just because a consumer loaded its ready cell.
[GPU control and payload separation](../gpu/pm4/handoff.md)
[Array output publication premises](../gpu/recipes/gpu-npu.md#output-dma-and-a-ready-flag)

Credit return is the reverse synchronization edge. For example, a GPU → NPU
input ring needs NPU → GPU evidence that the last external MM2S read ended;
an NPU → GPU output ring needs GPU → NPU credit return after the GPU
finishes reading the prior NPU output. A CPU credit owner similarly
completes its reads before releasing the slot. The return message can be
much smaller than the payload, but its ordering obligation remains. A source
copied into retained local storage can return credit before computation on
that copy finishes. [External and local DMA
lifetime](../xdna/dma.md#final-use-and-architecture-boundaries)

Generation values distinguish a new use from a stale observation. A protocol
using ordinary unsigned comparison has a non-wrapping observation interval.
A modular protocol defines its maximum outstanding distance and comparison
rule. Resetting a generation or rebasing its meaning requires excluding old
readers. These are protocol premises; choosing a wide integer does not supply
atomic access to it across the complete route.
[Control widths and generation](../gpu/recipes/gpu-npu.md#resident-programs-and-per-generation-ownership)

### Slots, cache lines and DMA extents

Slot independence is determined by all accesses to the backing, including
cache maintenance, vector loads, DMA burst/length rules and any programmed
repeat. A byte range which has returned credit may share a cache-maintenance
extent with a neighboring range still owned by a device. The CPU/NPU native
paths can expand a requested range to pages or a full scatter/gather object;
therefore cache-line padding alone does not establish independence for every
path. A mapping with suitable coherent/uncached access or separate native
maintenance owners changes that constraint. [Host/array cache contract](cpu-npu.md)

The array's external-source lifetime also follows the installed address
generator. An MM2S descriptor that repeats a source is still borrowing it
after the first stream object arrives. For a strided transfer, the accessed
set follows dimensions, strides, iteration and wrapping, rather than a
single contiguous logical tensor size. [Descriptor representation](../xdna/dma.md)

## Split, join and nested pipelines

A broadcast gives several readers a borrow of the same input version. Its
source can be reused after all those readers finish. A split gives readers
disjoint slices, potentially with different completion points. A join exposes
one composite output only after every required contributor has published its
portion. Whether those portions occupy separate cache-maintenance extents is
an additional constraint.

IRON's `distribute_and_join_L2` is a concrete local example: three workers
process separate slices of a memory-tile object, and lock-governed DMA gathers
them into a complete output. The awaited output depends on each input slice's
consumer. The switches route bytes; the workers perform arithmetic. Circuit
multicast adds backpressure from all destinations rather than an implicit
arithmetic reduction. [Complete split/join flow](../xdna/interconnects.md#a-split-compute-and-join-flow)
[IRON caller][split-join]

When GPU waves contribute to one external payload, their publisher joins
those contributions before the ready update. A workgroup barrier joins that
workgroup; it does not join other workgroups. When several array output
channels contribute, completion of one channel covers that channel's work,
not the union. The external publication point is downstream of all required
contributors. [GPU contributor join](../gpu/recipes/gpu-npu.md#publication-inside-a-gpu-shader)
[DMA channels and tasks](../xdna/dma.md#task-queues-and-completion-tokens)

Nested pipelines preserve the same rule. An inner pipeline can return an input
borrow as soon as it has copied every needed byte into storage it retains;
its final output completion can occur much later. An outer pipeline can
depend on that final output without inheriting ownership of the inner
pipeline's executable, local pools or routes. Those resources still belong
to the inner program until its independent last uses have ended.

One awaited leaf can retire earlier work only when a real dependency connects
them. In IRON's finite SAXPY flow, the output requires both inputs, so its wait
can precede recycling the two input tasks. `finish_task_group` orders waits
before frees, but cannot create that data dependence for unrelated transfers.
Freeing descriptor bookkeeping is distinct from hardware completion.
[SAXPY data dependence][saxpy] [Task-group owner][task-group]

## Progress and backpressure

Storage capacity and execution capacity are separate resources. A producer
can have a writable slot while its consumer has no runnable workgroup, DMA
queue entry or stream capacity. Conversely, a free task-queue entry does not
mean the active transfer stopped using its descriptor or source.
[Array queue and completion](../xdna/dma.md#task-queues-and-completion-tokens)

HSA 1.2 distinguishes base-profile from full-profile scheduling guarantees;
the base profile does not promise independent progress across queues. Its
workgroup progress rules also do not promise that every workgroup in one
dispatch runs concurrently. A resident workgroup waiting for a not-yet-active
workgroup therefore needs a scheduling argument beyond atomic visibility.
[HSA §§2.10–2.11][hsa-progress]

ROCr gives a concrete transfer restriction: an async copy's dependency list
excludes the completion of a future async-copy submission, since native queue
placement can deadlock that arrangement. Submission order is not a universal
replacement for explicit dependencies, and a visible wait value does not
guarantee that its producer can execute. [Copy dependency contract][async-copy]

Inside the array, cyclic channels need enough initial free objects and a
reachable sequence of producers/consumers. Multicast receivers share source
backpressure; a stalled branch can hold up the others after finite buffering
fills. These flow-control properties belong in the graph's progress argument
alongside dependencies, rather than being treated as cache behavior.
[Stream flow control](../xdna/interconnects.md#ports-and-circuit-routes)
[Local object ownership](../xdna/dma.md#local-locks-and-descriptor-chains)

## Drain, errors and resource retirement

A normal drain first closes new admission while allowing already admitted
work and credit returns to complete. Every waiter then reaches its termination
path through the mechanism on which it actually waits. An array worker blocked
on a stream needs a stream-level wake/termination protocol; writing an
unrelated stop cell does not satisfy its blocking read.
[Program quiescence](../xdna/execution.md#placement-and-program-quiescence)

| Resource | Last use before reuse or destruction |
| --- | --- |
| External source version | Every shader or external DMA read of that version, including repeats and broadcast readers. |
| External destination version | Successful producing completion and every downstream consumer's last read. |
| Ready/credit storage | Its last producer update and every waiter that can still interpret that generation. |
| Code, arguments and descriptors | The last hardware fetch or execution using those bytes, including indirect and queued references. |
| Array pools, locks, routes and installed BDs | Every worker and DMA channel that can revisit them has become quiescent. |
| Native mappings and backing | All users of the addresses and bytes, followed by the native owners' unmap, unlock, close or free sequence. |

[GPU resource owners](../gpu/recipes/host-device.md#imported-buffers-and-final-use)
[Array resource owners](../xdna/dma.md#final-use-and-architecture-boundaries)
[External handle owners](external-memory.md)

Command success, failure and termination have different consequences. The
XDNA native fence accompanies terminal error as well as success, and ROCr
copy completion can report an error by a negative signal. A failed producer
does not publish a valid output or justify waiting indefinitely for an ordinary
success flag. The graph's failure owner stops admission and resolves dependent
work through its actual native error/termination path while retaining storage
still reachable by accepted work. A host timeout itself does not cancel that
work. [XDNA status and wait](../xdna/execution.md#acceptance-completion-and-result-inspection)
[Copy result contract][async-copy]

## Measuring a handoff

The measured interval identifies the cost being compared:

| Interval | Start and end |
| --- | --- |
| Host publication | Command/data preparation boundary through native publication; final device completion is outside the interval. |
| End to end | Host invocation through successful completion and consumer-visible output, including any host cache acquisition. |
| Device handoff | Producer release through the consumer's acquired payload use, with named execution points at both ends. |
| Source occupancy | First ownership of a source slot through returned credit; this determines how much buffering is required. |
| Drain | Admission close through the last relevant worker, transfer, consumer and notification retirement. |

CPU, GPU and tile counters are separate clock domains. Cross-device subtraction
requires retained correlation samples, units, effective widths and reset epochs,
plus uncertainty. A GPU calibration does not align an array timer, and a
timestamp before an asynchronous transfer measures its issue point rather than
its completed payload. A dependency can establish event ordering without
establishing a precise duration. [GPU clocks](../gpu/observability.md#clock-domains-and-conversion)
[Array host brackets](../xdna/observability.md#relating-a-sample-to-host-time)

For a steady stream with fixed positive issue interval `I` and positive
source occupancy bounded by `L`, `ceil(L / I)` is the capacity needed just
to retain those in-flight source versions under that bounded schedule.
At an equal boundary, allocating before processing returned credit can
require one additional slot. Bursty issue or longer occupancy needs a bound
covering that behavior. This estimates logical slots; allocation and
maintenance granules determine their physical storage extent. It is a
scheduling model rather than a hardware queue-depth guarantee. The slowest
sustained compute, transfer or credit path constrains throughput; overlap
changes how much of each latency is exposed on the critical path.

Transfer and cache work contribute separate byte counts. A useful-payload rate
can differ from traffic through DRAM, a GPU cache or array streams because of
staging, repeated reads, padding and maintenance. Timestamp and counter reports
retain those units and attribution so that a shorter host submission is not
mistaken for a faster completed heterogeneous pipeline.
[GPU counter interpretation](../gpu/observability.md#event-identity-and-aggregation)
[Array events](../xdna/observability.md#event-counters)

[async-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2153
[hsa-progress]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf#page=32
[split-join]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/programming_guide/section-2/section-2f/05_join_L2/distribute_and_join_L2.py#L29-L83
[saxpy]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/programming_examples/getting_started/01_SAXPY/saxpy.py#L63-L95
[task-group]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/runtime/runtime.py#L89-L155
