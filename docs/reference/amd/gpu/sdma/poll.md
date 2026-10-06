# SDMA memory dependencies

A memory poll delays following SDMA work until its comparison is satisfied.
The producer's completion protocol, visibility of the control word and payload
cache transitions are separate contracts. A shader-to-SDMA handoff needs the
shader's release before publishing completion, the matching SDMA acquire where
required, and storage that remains live through the dependent operation. The
[cache reference](cache.md) and [cross-engine recipes](../recipes/README.md)
describe those surrounding operations.

## Classic memory equality

ROCr's ordinary unscoped builder emits this six-DWORD POLL_REGMEM form. The
operand is a readable, DWORD-aligned memory word; addresses are byte
addresses. Register polling uses a separate address convention and selector.
[ROCr builder][rocr-poll] [Linux memory/register split][linux-poll] [PAL
layout][pal-layout]

| DWORD | Field | ROCr memory-dependency value |
| --- | --- | --- |
| 0 | Opcode bits 7:0; suboperation bits 15:8 | 8; 0. |
| 0 | HDP selector bit 26; comparison bits 30:28; memory selector bit 31 | 0; equality 3; 1. Other control fields remain zero. |
| 1–2 | Address low/high | Full address of the aligned control word. |
| 3 | Reference | Exact 32-bit value; the AQL-completion recipe waits for zero. |
| 4 | Mask | `0xffffffff`, comparing the full word. |
| 5 | Interval bits 15:0; retry count bits 27:16 | 4; `0xfff`, the classic infinite-retry value. Bits 31:28 remain zero. |

ROCr and Linux use interval 4. PAL uses `0xa` and describes that setting as
160 clocks; Mesa selects the same named interval. This does not establish a
universal nanosecond period for interval 4. The infinite-retry setting keeps
valid asynchronous work independent of a device-side deadline. Finite retry
exhaustion and its error/completion behavior are not specified by these
callers. [PAL wait][pal-wait], [Mesa wait][mesa-wait]

The ordinary ROCr, PAL and Linux memory-equality callers use full masks.
Mesa's SDMA gang postamble additionally uses comparison 5, greater-or-equal,
with reference 1 and a full mask. Its comparison selector occupies the same
header bits 30:28 as equality. That caller gives no signed-ordering or
counter-wrap contract and uses no partial mask. The explicit masked-operand
formula in ROCr's newer 64-bit packet definition is evidence for that form,
not a substitute for a
classic partial-mask contract. [Mesa terminal join][mesa-join], [comparison
value][mesa-comparisons], [64-bit definition][poll64-layout]

## Signal lifetime and actual callers

ROCr's ordinary user-ring submission samples each dependent 64-bit signal. It
omits already-zero dependencies, otherwise polls a nonzero high word to zero
before polling the low word. This relies on the signal's producer and reuse
protocol; two separate 32-bit reads are not an atomic 64-bit comparison.
Applicable HDP maintenance and USER_GCR acquire follow the dependencies before
the copy body. The poll itself supplies neither operation. [Dependency
construction][rocr-deps], [submission ordering][rocr-order]

The HSA async-copy contract requires system-coherent buffers, with producer
release and receiving-device acquire as applicable. Copy completion has its
own signal update. It also disallows an async-copy dependency on a future
async-copy submission because the queue arrangement can deadlock. That rule
does not establish arbitrary consumer-first progress between two SDMA queues.
[Public copy contract][hsa-copy]

PAL waits on GPU-event storage; Linux's pipeline wait observes the kernel
ring's own fence sequence. Those scheduled-command callers corroborate the
poll, while their submission wrappers retain separate cache responsibilities.
They do not establish a persistent KFD user ring's cache recipe. [PAL event
wait][pal-wait], [Linux pipeline wait][linux-wait], [user versus scheduled
cache ownership](cache.md)

Mesa's SDMA gang leader explicitly joins ACE completion before the kernel can
signal final submission completion and permit command-buffer reuse. The ACE
producer uses a confirmed end-of-pipe store. This demonstrates the distinction
between a data dependency and the terminal join that protects every
outstanding owner; a leader's earlier progress alone is insufficient. [Mesa
gang completion][mesa-join], [PM4/SDMA ownership
recipe](../recipes/pm4-sdma.md)

## Generation and transport distinctions

| Source-selected form | Exact condition and consequence |
| --- | --- |
| ROCr host polling workaround | ISA major 9, minor 0, stepping **not 10**. ROCr waits through host asynchronous signal handlers instead of issuing dependency polls, citing premature poll completion. gfx942 is outside this predicate. The branch gives no firmware cutoff or universal guarantee about other devices. [Predicate][rocr-workaround], [consumer][rocr-deps] |
| ROCr scoped classic poll | `scopeFields` adds SYS scope at DWORD 5 bits 29:28. The non-DXG factory selects this variant for ISA major 11 or 12 with minor at least 5. PAL's classic and GFX12 definitions reserve these bits. This remains a source disagreement, including gfx115x; it is not resolved by a compiler target name. [Factory][rocr-factory], [builder][rocr-poll], [PAL classic][pal-layout], [PAL GFX12][pal12-layout] |
| PAL GFX12 memory poll extent | PAL's structure includes a seventh `GRBM_GFX_INDEX` word; its mode-0 memory caller zeroes and emits the complete structure. Linux SDMA7.0/7.1 memory pipeline waits emit six DWORDs. Whether the extra word is conditional data, padding or a source-layout discrepancy is unresolved by these callers. [PAL caller][pal12-wait], [layout][pal12-layout], [Linux7.0][linux70-wait], [Linux7.1][linux71-wait] |
| ROCr GFX1250 `POLL_MEM_64B` | Eight DWORDs, opcode 8/suboperation 5, QWORD-aligned address, 64-bit reference/mask and an eight-bit retry field in DWORD 7. **Zero** means infinite retry; there is no classic interval field. The builder selects system memory and optional SYS scope. [Layout][poll64-layout], [builder][poll64-builder] |

The last form is used for additional dependencies in ROCr's indirect
linear-copy wait/signal path, separately from its ordinary classic dependency
loop. Initialization selects that service for ISA major 12 with minor at least
5. This is a different packet and caller, with its own layout and retry
semantics. [Service predicate][rocr-service], [64-bit caller][poll64-caller]

Compute ISA, native SDMA IP, transport, and the selected runtime template
identify different parts of these protocols. In particular, scope fields are
operation-specific: COPY_LINEAR and FENCE do not establish POLL_REGMEM's
layout.

## Programming sequence and lifetime

For a one-producer completion cell, the producer writes its payload, performs
its required release, and then publishes the terminal value. The SDMA queue
polls that value, performs the required acquire, and reads the payload. A
later final-use join protects the cell while any waiter can still read it. The
native [copy-completion protocols](atomics.md) show the actual release,
signal-update, and last-consumer owners.

A 64-bit signal represented by two classic DWORD polls also needs a stable
value/reuse protocol. The high/low sampling sequence above does not supply an
atomic 64-bit timeline observation. Retaining a terminal value through all its
waiters and rearming only after their final use avoids confusing one
operation's completion with the next operation's initial state.

[rocr-poll]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2640-L2659
[rocr-deps]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L397-L442
[rocr-order]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L567
[rocr-workaround]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L262-L266
[rocr-factory]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L884
[rocr-service]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L204-L207
[hsa-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2136
[pal-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2744-L2816
[pal-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L70-L104
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2887-L2965
[pal12-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L88-L119
[mesa-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L47-L57
[mesa-comparisons]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L90-L93
[mesa-join]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1420-L1457
[linux-poll]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L392-L415
[linux-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1286-L1295
[linux70-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1169-L1184
[linux71-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1175-L1189
[poll64-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L795-L871
[poll64-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2662-L2687
[poll64-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L742-L746
