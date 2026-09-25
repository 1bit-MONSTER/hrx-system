# Indirect compute dispatch

DISPATCH_INDIRECT reads launch dimensions from GPU memory while using the
currently bound compute program and resource state. The count tuple is a CP
input. Any grid-size values consumed by the shader are a separate compiler ABI
obligation. The producer-to-fetch dependency and the tuple's last consumer
must both be known before its storage can be changed.

## Packet and count representation

The ordinary MEC form is opcode `0x16`, four DWORDs: type-3 header, absolute
address low/high and DISPATCH_INITIATOR. PAL requires four-byte alignment and
uses three consecutive unsigned 32-bit X/Y/Z workgroup counts. A zero dimension
discards the dispatch in its public contract. The ME form is three DWORDs and
uses a 32-bit byte offset relative to an address established by SET_BASE; it is
not the same address operand. [MEC builder][pal-mec] [ME builder][pal-me]
[Argument layout][pal-args] [Caller contract][pal-api]

PAL's ordinary MEC builder leaves USE_THREAD_DIMENSIONS clear and sets
COMPUTE_SHADER_EN, FORCE_START_AT_000 and ORDER_MODE, plus the program's wave32
and selected tunneling/preemption controls. NUM_THREAD_X/Y/Z still describe
the local group. Thus a tuple `(16,1,1)` with local `(64,1,1)` requests 1024
workitems, not sixteen. RADV also has an explicitly `unaligned` internal path
that sets USE_THREAD_DIMENSIONS and programs full local dimensions; that path
must not be conflated with the ordinary group-count API.
[PAL initiator][pal-mec] [RADV dispatch construction][mesa-emit]

The pinned producers use different low header bits: PAL's type-3 defaults yield
`0xc0021600`; RADV ORs the compute shader bit and emits `0xc0021602`. PAL's MEC
header names the low byte reserved, whereas its shared header builder supports
ME/PFP shader-type fields. These actual conventions are recorded separately;
one does not establish that every low-bit permutation is interchangeable.
[Header definition][pal-header] [PAL defaults][pal-defaults]
[RADV emitter][mesa-emit]

RADV sets ORDER_MODE for its selected GFX7+ devices and clears it for ordered
dispatches. Compute-only GFX940+ takes a different default from graphics-capable
devices. Tunneling is another separately selected policy. ORDER_MODE supplies
neither execution completion nor payload visibility.
[Default policy][mesa-policy] [Per-dispatch policy][mesa-emit]

## Shader-visible inputs

PAL's PAL-ABI path checks `numWorkGroupsRegAddr`. If present, it writes the
indirect tuple's address to the ABI-selected user SGPRs; a direct launch instead
materializes its logical dimensions in embedded memory. RADV checks
`AC_UD_CS_GRID_SIZE` and loads all three values into user SGPRs with
LOAD_SH_REG_INDEX on GFX10.3+, otherwise passing their pointer. The CP's
indirect fetch does not perform these ABI operations implicitly. RADV records
hangs on older compute queues even though LOAD_SH_REG_INDEX exists on GFX8+;
its predicate retains that compute-engine restriction.
[PAL user data][pal-abi] [RADV grid input][mesa-grid]
[SGPR policy][mesa-grid-policy]

Other compiler inputs can include dispatch pointers, hidden grid sizes,
preloaded arguments or scratch state. They must match the actual entry ABI.
PAL's public indirect-dispatch API expressly excludes HSA-ABI pipelines;
that is an implementation boundary of this caller, not a hardware prohibition
on all manually prepared HSA entries. [PAL API boundary][pal-api]
[Pipeline selection][pal-selection] [Compiler input rules][llvm-inputs]

## Publication and last use

The tuple is borrowed storage, not a copied packet operand or an atomic
12-byte snapshot. It remains stable until every CP fetch and shader reader
using it completes. A scalar load through an ABI-provided pointer may extend
its lifetime beyond command parsing. Code, arguments and payload allocations
have their own last-use obligations.

PAL classifies indirect arguments as GL2 clients. A CPU producer is a
GL2-bypassing actor in its barrier model, so publication toward a GL2 consumer
requests GL2 writeback/invalidation. A shader that also reads the tuple can
require its scalar/vector cache acquire in addition to the CP fetch edge.
[Access classes][pal-cache] [CPU-to-GL2 mapping][pal-cpu]
[Indirect acquire stage][pal-indirect-acquire]

For GPU-produced counts, the ordinary sequence is:

```text
producer shader writes the complete valid tuple
  → join producer execution → perform producer/consumer cache operations
  → bind the consumer's complete ABI → DISPATCH_INDIRECT
  → complete the consumer and its output publication
  → retire every remaining tuple, argument and command-storage reference
```

PAL's barrier lowering waits for compute execution before trailing cache work.
The compute acquire point is ME; there is no PFP dependency to borrow. A plain
ACQUIRE_MEM cannot replace the producer join, and observing the ring read
pointer cannot replace the final consumer's completion.
[Execution before cache work][pal-cs-publish]
[Indirect acquire placement][pal-indirect-acquire]

The tuple's count units do not bound shader data accesses by themselves.
Backing must cover the work launched by the selected dimensions and the
program's actual argument-dependent footprint. A shader's internal bound may
reduce payload accesses without changing the hardware workgroup count.

## Generation-specific differences

RADV's asynchronous-compute alignment repair is specifically for its GFX7
predicate. If a tuple is not 32-byte aligned, it allocates aligned upload
storage and copies the three words with confirmed COPY_DATA before dispatch.
That workaround does not change the ordinary four-byte packet/API alignment
on later generations. [Bug predicate][mesa-align] [Actual repair][mesa-emit]

PAL's GFX12 MEC builder retains a four-DWORD absolute-address packet. This
representation continuity does not make GFX12 resource registers, cache
operations or preemption policy identical to GFX10/GFX11.
[GFX12 builder][pal-gfx12] [Direct binding](dispatch.md)

[pal-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1538-L1572
[pal-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1275-L1301
[pal-args]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L1404-L1410
[pal-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3051-L3070
[mesa-emit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15035-L15149
[pal-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L54
[pal-defaults]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L805-L810
[mesa-policy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1640-L1653
[pal-abi]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L719-L734
[mesa-grid]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15083-L15099
[mesa-grid-policy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2796-L2797
[pal-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L160-L167
[llvm-inputs]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6920-L7004
[pal-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L56-L68
[pal-cpu]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L352-L364
[pal-indirect-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L534-L570
[pal-cs-publish]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L2027-L2042
[mesa-align]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1021-L1037
[pal-gfx12]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1328-L1360
