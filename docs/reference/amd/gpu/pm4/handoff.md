# Cross-queue compute handoff

A producer queue's completion and a consumer queue's visibility are different
operations. The producer must finish the relevant shader work and publish its
stores before signaling. The consumer waits for that signal, then acquires
the caches through which it will read the payload. Queue submission order alone
does not create this dependency.

PAL's event-based release/acquire contract supplies this complete protocol:
release completes the source synchronization scope and availability work before
setting the event; acquire waits early enough for the destination stage and
then performs visibility and layout operations. [Event contract][pal-event]

## Representation and ordering

For ordinary compute payloads the graph is:

```text
producer dispatch → producer execution join and release → event store
                                                        ↓
consumer memory wait → consumer cache acquire → consumer dispatch
  → terminal consumer completion
```

PAL's `ReleaseEvent` routes through the release planner. Its `AcquireEvent`
emits a full-mask equality WAIT_REG_MEM on each event slot and then calls
`AcquireInternal`. The packet wait precedes, rather than contains, the payload
visibility operation. Mesa's ordinary memory wait likewise has explicit
address, reference, mask and poll interval fields.
[Release implementation][pal-release] [Acquire implementation][pal-acquire]
[Wait representation][mesa-wait]

The signaling stage matters. PAL uses an EOP release when the event's stage
includes compute or bottom-of-pipe, while CP-stage events use confirmed
WRITE_DATA. Outstanding CP DMA has a separate wait requirement. A CP write
placed after an asynchronous dispatch is not automatically a shader-completion
signal. [Stage-specific event write][pal-event-write]

The control cell must support the producer's store and the waiting engine's
fresh read before any *later* payload acquire. A cache invalidate placed after
a stale control poll cannot make that poll progress. Payload mapping and
control mapping may therefore carry distinct requirements even when both
occupy system memory.

## A concrete native mapping boundary

Linux's GMC11 path provides one explicit composition for owned GTT: KFD
COHERENT/UNCACHED flags become GEM flags and select GPU MTYPE_UC. TTM selects
CPU-cached backing unless CPU_GTT_USWC is set; cached GTT receives SYSTEM and
SNOOPED PTE treatment. CPU write-back mapping and GPU uncached mapping are
compatible properties, not contradictory labels.
[Allocation flags][linux-flags] [GPU PTE policy][linux-mapping]
[CPU cache choice][linux-cpu] [System snooping][linux-pte]

These source predicates identify the mapping mechanism. They do not replace
producer stage completion, expand to arbitrary imported/local memory, or
establish an atomic multiwriter protocol. The event and payload still require
the cache operations selected for their actual clients. PAL's barrier model
separates GL2 clients from CPU/memory actors that bypass GL2.
[Access classes][pal-actors] [Cross-client cache operations][pal-cache]

## Owners and a complete sequence

The producer owns its inputs and arguments through producer completion. An
intermediate payload remains borrowed by the consumer after the producer is
done. The event cell remains borrowed through the consumer's wait, and the
consumer's arguments/output remain live through its own completion.

A reusable handoff therefore proceeds as follows:

1. Establish the native mappings and initial event state; make CPU-authored
   commands, arguments and inputs visible before publishing either queue.
2. Publish the consumer wait/acquire/work sequence and the producer work/
   release/signal sequence using each queue's producer protocol. Independent
   progress is a scheduling premise, not proof of simultaneous shader execution.
3. Observe the final consumer completion using its specified visibility
   contract before reading its output.
4. Join any independently outstanding producer or notification work, retire
   both command ranges, and only then rearm control cells or overwrite storage
   still referenced by either stream.
5. Release mappings only after all submitted users and queue ownership have
   ended.

The event contract establishes the execution/cache edges. The submission owner
must separately account for every queued reference and any work following the
signal. PAL's retained command-buffer reset contract explicitly requires no
queued/executing use and no remaining nested reference; its command-storage
postamble joins shader users before updating retirement state.
[Retained reset contract][pal-reset] [Command retirement][pal-retirement]

A terminal consumer completion can close the payload dependency while a later
notification or command tail remains live. Conversely, command consumption
only retires command bytes; it does not grant permission to overwrite payload
that another queue still consumes. [Command-buffer ownership](command-buffers.md)

[pal-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2818-L2862
[pal-release]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L2600-L2612
[pal-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L2614-L2656
[mesa-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L89-L103
[pal-event-write]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1327-L1383
[linux-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1748-L1783
[linux-mapping]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c#L468-L513
[linux-cpu]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1190-L1214
[linux-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1432-L1476
[pal-actors]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L56-L68
[pal-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L353-L365
[pal-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2244-L2306
[pal-retirement]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1230-L1267
