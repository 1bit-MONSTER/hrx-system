# PM4 compute commands

PM4 is the command processor's packet language. A compute stream binds shader
state, launches work, moves data, and establishes dependencies between execution
stages and memory clients. The stream's transport also owns its address space,
queue context, publication protocol, and command-storage lifetime. Those facts
are not encoded by an opcode alone.

This section follows ordinary MEC compute callers in PAL, Mesa and Linux/KFD.
ME and PFP forms are identified separately where the packet or ordering rules
differ. PAL's `gfx9` source directory contains definitions for later generations;
the directory name is not an architecture predicate. [Packet definitions][packets]
[Generation predicates][generations]

## Programming sequence

A complete compute operation has several owners:

1. The native queue owner establishes the process VM, scheduling context and
   runtime-controlled shader state.
2. The program owner supplies a compatible executable, its complete readable
   fetch extent, resource requirements and initial-register ABI.
3. The submission owner makes command bytes, arguments and input data visible
   before publishing the queue or indirect-buffer reference.
4. The command processor binds the program and launches work. Dependencies join
   the actual producer stage before applying the required cache operations.
5. A completion protocol publishes a result to its intended observer. Command
   consumption and the last dependent consumer separately determine which
   storage can be reused.

PAL's compute postamble illustrates why the distinctions matter: it separately
drains CP DMA, waits for shaders that may access command memory, then increments
its command-storage tracker. That tracker relies on a subsequent KMD cache-
flushing EOP in PAL's scheduled submission path. A raw user ring has no implicit
right to that trailer. [Compute postamble][postamble]

## Topics

| Chapter | Native mechanism |
| --- | --- |
| [Queue publication](publication.md) | Ring capacity, DWORD frontiers, host visibility, doorbells and the distinct KFD, scheduled DRM and DRM userq owners. |
| [Memory commands](memory-commands.md) | COPY_DATA, WRITE_DATA, waits and shader-completion firmware predicates. |
| [Atomic operations](atomics.md) | TC integer operations, returned values, command modes, participant domains and cache/retirement contracts. |
| [Cache control](cache.md) | Acquire/release fields, native-generation differences, ranges, scopes and complete visibility sequences. |
| [Compiled dispatch](dispatch.md) | Executable backing, register and argument ABI, MEM_ORDERED wait-counter mode, direct launch, runtime state and completion. |
| [Group memory](lds.md) | Static and dynamic LDS allocation, workgroup synchronization and resource rebinding. |
| [Indirect dispatch](indirect.md) | Memory-resident workgroup counts, compiler inputs and producer-to-fetch dependencies. |
| [Command buffers](command-buffers.md) | First-level INDIRECT_BUFFER entry/return, publication and completed-use rebuild. |
| [Cross-queue handoff](handoff.md) | Release, control signaling, wait, consumer acquire and last-use ownership. |
| [Command-processor DMA](dma.md) | DMA_DATA copies, fills, prefetch, completion discrepancies and cache routing. |
| [Timing](timing.md) | Sampling stage, timestamp visibility, clock domains and profiling ownership. |
| [Performance counters](counters.md) | Event and instance selection, register fields, sample widths, collection sequencing and completed-use result ownership. |
| [Performance queries](counter-queries.md) | RADV/Vulkan profiling locks, private submission serialization, counter-pass layout, result decoding and native clock-owner lifetime. |

[Architecture identity](../architectures.md) distinguishes compiler targets,
native IP versions and firmware. [AQL](../aql/README.md) and
[SDMA](../sdma/README.md) are different packet/transport contracts; sharing a
memory allocation does not make their completion or cache operations
interchangeable. [Programming recipes](../recipes/README.md) compose those
contracts across actors.

## Interpreting the source

A packet definition establishes representation. A builder selects fields. A
caller establishes the execution sequence, and the allocation/submission owner
establishes publication and retirement. The chapters keep those layers visible.
They also retain disagreements: MEC and graphics ACQUIRE_MEM range widths,
PAL and Mesa DMA completion conventions, and metadata-cache policies cannot be
resolved merely by choosing a familiar packet name.

Execution completion, write confirmation, cache visibility, notification and
command retirement answer different questions. In particular, an instruction
cache invalidate does not retire an older shader, a memory wait does not by
itself acquire payload caches, and a queue read pointer is not a shader result.
The applicable generation, engine, firmware and mapping predicates belong to
each complete sequence. [Compute waits][idle] [Cache-only acquire][acquire]

[packets]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L136
[generations]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L2328-L2333
[postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1230-L1267
[idle]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4284-L4337
[acquire]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L398-L427
