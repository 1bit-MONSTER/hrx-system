# SDMA atomic operations and signaling

SDMA has several ordinary signaling protocols with different owners and memory
contracts. ROCr uses `ATOMIC` to decrement copy-completion signals. PAL uses
`MEM_INCR` to retire command storage. The legacy Radeon CIK driver uses
`SEMAPHORE` to join rings. An operation's packet layout, payload visibility and
final storage use are separate parts of each protocol.

## Applicability

The protocols below describe distinct native consumers. The detailed sections
state their engine, transport, memory, and ownership conditions.

| Protocol | Encoding | Native consumer |
| --- | --- | --- |
| ROCr completion decrement | Opcode 10, operation 47 (`ADD64`), eight DWORDs. | An HSA signal observer or dependent operation. |
| PAL command retirement | Opcode 7, suboperation 1 (`MEM_INCR`), three DWORDs. | The command allocator's root-chunk busy tracker. |
| CIK ring handshake | Opcode 7, suboperation 0 (`SEMAPHORE`), three DWORDs. | A waiting ring, with semaphore storage retained through the consumer fence. |
| gfx125 fused copy signaling | COPY_LINEAR with optional WAIT/SIGNAL blocks. | The selected copy path's dependency and completion signals. |

## Copy completion through ADD64

The HSA async-copy API requires both agents to access both buffers, valid
nonoverlapping extents and completion of all dependency signals before the
copy. Completion decrements the output signal; negative values represent
asynchronous errors. The caller must ensure system-level coherent buffers
because the DMA engines may sit outside the same coherency domain. In general,
this requires a sending-device SYSTEM release before the copy and a recipient
SYSTEM acquire before consuming its output. An atomic control update does not
replace those payload obligations. [Public copy contract][copy-api]

ROCr's classic submission builds this complete flow:

```text
dependency polls → selected HDP/cache acquire → copy → selected cache release
  → gang joins, if any → output decrement/store → optional mailbox FENCE/TRAP
```

[Stream construction][copy-stream] [Notification tail][completion]

`BlitSdma::Initialize` selects atomic completion from the directed link between
the GPU node and `cpu_agents()[0]`: `atomic_support_64bit`, except gfx701
explicitly disables it. Initialization rejects a non-GPU agent and the FULL
HSA profile. This is the actual source predicate, not a guarantee for every
integrated GPU, memory attachment or atomic operation. Topology initialization
applies the link's `Override` and `NoAtomics64bit` flags. Its comment about
disallowing XGMI overrides disagrees with the unconditional `if (Override)`
code; the predicate follows the code. Linux derives directional PCIe atomic
flags separately and exempts XGMI from that check.
[Runtime selection][atomic-gate] [Link interpretation][link-flags]
[Kernel directionality][linux-links]

When the platform predicate is true, the output update is ADD64 with operand
`UINT64_MAX`. Otherwise ROCr computes `LoadRelaxed() - 1` on the CPU and emits
the necessary high-word FENCE followed by low-word FENCE. Its source explicitly
uses FENCE because consecutive copy/write commands may overlap. That alternate
path supplies neither a generic concurrent read-modify-write nor an indivisible
64-bit store. [Completion construction][completion] [Fence choice][fence-choice]

### Representation

The ordinary decrement builder zeroes all eight DWORDs before filling these
fields. The signal owner provides a 64-byte-aligned structure with its signed
64-bit value at byte offset 8, giving the selected target eight-byte alignment.
[Builder][atomic-builder] [Layout][atomic-layout] [Signal storage][signal-layout]
[Opcode, operation and scope constants][atomic-constants]

| Word or field | Ordinary decrement value and meaning |
| --- | --- |
| Header bits 7:0 / 15:8 | Opcode 10 / suboperation 0. |
| Header bit 16 / bit 18 | Loop disabled / TMZ disabled. Reserved bits remain zero. |
| Header bits 31:25 | Operation 47, ADD64. |
| Words 1–2 | Full byte address of the signal value. |
| Words 3–4 | Low/high source operand, both `0xffffffff`. |
| Words 5–6 | Comparison operand, both zero; this caller performs no comparison. |
| Word 7 bits 12:0 | Loop interval zero; the remaining bits are zero. This loop-disabled caller establishes no interval unit for a looping operation. |
| ROCr header bits 21:20 | SYS scope when the scoped template is selected; zero otherwise. |
| ROCr header bits 24:22 | Temporal hint zero. |

There is no separate returned-value address in this form. The caller consumes
the updated cell, not the old value. The operation number alone does not
establish other operations, widths, looping semantics or return transport.

### Architecture and transport differences

The ROCr factory selects these templates. The aliases determine whether the
builder emits USER_GCR and scope fields. Cache maintenance for the copied
payload remains independent of the signal update. [Factory][factory] [Template aliases][aliases]

| Source predicate | Template behavior |
| --- | --- |
| gfx9, including gfx942 | V4: no USER_GCR and no scope fields. The directed atomic-link predicate still applies. |
| gfx10 outside DXG | V5: USER_GCR, without scope fields. |
| gfx11/12 outside DXG, minor < 5 | V5. |
| gfx11/12 outside DXG, minor >= 5 | V6: scope fields, without USER_GCR. |
| gfx10/11/12 on DXG | V4; the factory assigns surrounding GCR work to the underlying driver. |

The V6 alias comment describes DACC/OSS7.1, but the factory also selects it for
gfx11.5. This matters for ATOMIC: the pinned Linux SDMA6 and PAL gfx103Plus
layouts name header bits 22:20 **cache policy** and bit 24 **CPV**, whereas
ROCr names bits 21:20 **scope** and bits 24:22 **temporal hint**. Matching bit
positions do not resolve that semantic discrepancy. The gfx11.5 factory choice
and shared opcode leave that difference unresolved; they do not identify which
interpretation applies to a particular SDMA6 firmware interface.
[Linux policy fields][linux-atomic] [PAL policy fields][pal-atomic]

HDP work has another predicate: the runtime setting, gfx major >= 9 excluding
gfx10.1, and a non-XGMI link. That test neither establishes a universal HDP
requirement nor supplies control-cell atomic reach. ROCr selects atomic
completion using the ISA, template, transport, and link conditions above. Its
separate HDP setting is sampled during initialization and checked again when
constructing the copy stream.
[HDP support][atomic-gate] [HDP setting][hdp-setting]
[HDP emission][hdp-emission]

### Memory and lifetime

ROCr appends any mailbox FENCE/TRAP after the completion decrement. Observing
the data-completion value therefore does not establish completion of that
notification tail. Ring availability is independently calculated from the
native read index and serialized producer commits. Ring-byte retirement does
not release payload or control storage still borrowed by another queue.
[Notification tail][completion] [Ring publication][ring-publication]

The gang path makes the last-consumer rule concrete: the leader polls each
internal signal to one, then performs its final decrement/store so that signal
destruction cannot race the poll. The successful batch path joins body signals
before final output completion and ties internal-signal destruction to that
completion. These are owner-specific lifetime protocols, not permission to
rearm any observed signal. [Gang ownership][gang-owner] [Gang commands][gang-commands]
[Batch ownership][batch-owner]

## MEM_INCR and command allocator retirement

PAL's SDMA `AddPostamble` emits an increment when the command stream has a
nonzero GPU busy-tracker address. The tracker belongs to the first command
chunk and is updated after command execution. The operation uses opcode
7/suboperation 1 with a header and full target address, checks eight-byte
alignment, and supplies no explicit operand, comparison, retry count or
returned-value address.
[PAL postamble][pal-postamble]

The root tracker reserves eight bytes, or uses separate writable storage for
read-only command streams. PAL compares only its low 32-bit count because
engine counter widths differ, under an explicit no-wrap assumption. Submit
counts cover root and nested uses. In PAL's automatic tracked reuse path,
`ReuseChunks` consults `IsIdleOnGpu`: matching submit/done counts or a retired
root generation permits the chunks to return to the free list. This establishes
PAL's GPU-increment/CPU-observation/storage-reuse flow, not a general full-width
MEM_INCR counter or CPU-concurrent arithmetic contract.
[Tracker construction and idleness][pal-tracker]
[Submission count][pal-submit] [Allocator reuse][pal-reuse]

The generation-specific policy fields also differ from ATOMIC:

| PAL layout | MEM_INCR policy and address fields |
| --- | --- |
| gfx103Plus | L2 policy at header bits 25:24, LLC policy at bit 26, CPV at bit 28; full address low/high words. The emitter fills policy only when MALL is supported. |
| gfx12 | MALL policy at header bits 27:26; low address represented as bits 31:3 with the bottom three bits zero, then the high word. |

The gfx10 backend's MALL choice depends on Navi2x and settings. CPV additionally
requires a non-default policy setting and valid KMD L2 policy. GFX12 uses
`GetMallPolicy(false)`: the destination MALL setting when MALL is supported,
otherwise policy zero. These are actual policy owners, not a universal packet
override. Payload cache visibility remains independent of allocator retirement.
[gfx103Plus fields][pal-increment-layout]
[Policy predicates][pal-policy] [gfx12 postamble][pal12-postamble]
[gfx12 fields][pal12-layout] [gfx12 policy selection][pal12-policy]

## Classic SEMAPHORE and ring handoff

Classic opcode 7/suboperation 0 also occupies three DWORDs, but its controls
are `write_one` at bit 29, `signal` at bit 30 and `mailbox` at bit 31. The
retained Radeon CIK caller selects only ordinary signal/wait: bit 30 is set
for signaling and clear for waiting, with the other two controls zero. Its
private eight-byte, eight-aligned cell starts at zero.
[Classic fields][semaphore-layout] [CIK emitter][cik-emitter]
[Cell owner][cik-owner]

The TTM paging-copy path finds producer dependencies, emits a signal on the
producer ring and a wait on the consumer ring, then performs its copy and
signals the consumer fence. Semaphore storage is freed behind that final
consumer fence. The software waiter count tracks emitted wait/signal balance;
it is not an exposed CPU arithmetic protocol for the cell. This is a complete
kernel-managed CIK handoff, with different VM/ring/storage ownership from
KFD user queues. [Copy caller][cik-copy] [Dependency and free][cik-sync]

RADV's `gang_sem_bo` illustrates why names do not identify packet semantics.
Its control allocation uses GL2_BYPASS. The SDMA gang leader waits through
POLL_REGMEM for the member's confirmed shader EOP store before KMD completion
allows command reuse. That protocol does not emit classic SEMAPHORE or create
an HSA signal object. [RADV gang owner][radv-gang]
[SDMA memory poll][mesa-poll]

## gfx125 fused wait/copy/signal

ROCr's selected on-engine path uses a distinct fused form when the ISA major
is 12 and minor >= 5. `DmaCopyOnEngine` reaches
`SubmitLinearCopyBodyWaitSignal`; this is an actual caller, not merely a packet
definition. [On-engine caller][fused-caller]

The packet consists of a header, an optional seven-DWORD WAIT block, six COPY
DWORDs and an optional five-DWORD SIGNAL block. Absent blocks are omitted,
not zero padded. The builder selects an eight-aligned EQ64-to-zero wait with
full mask and SYS scope, a bytes-minus-one copy count with SYS source/destination
scopes, and signal operation `0x70`, 64-bit subtraction with operand one.
[Fused builder][fused-builder]

Its single-copy caller adjusts the initial completion count for the number of
chunks. The shared builder also supports boundary-only waiting/signaling, with
wait on the first chunk and signal on the last. Those are different caller
protocols. The generation has separate POLL_MEM_64B (opcode 8/suboperation 5,
retry zero meaning infinite) and FENCE_64B (opcode 5/suboperation 2, SYS/MTYPE3)
builders. Neither is the classic poll's `0xfff` retry spelling or two ordered
FENCE32 stores. [Chunk ownership][fused-count]
[64-bit poll/fence][wide-controls]

[copy-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2153
[copy-stream]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L635
[atomic-constants]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L50-L81
[atomic-gate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L156-L204
[link-flags]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_topology.cpp#L220-L289
[linux-links]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L1210-L1236
[completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L663
[fence-choice]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[atomic-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2944-L2958
[atomic-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L873-L938
[signal-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_signal.h#L49-L77
[factory]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L889
[aliases]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L578-L590
[linux-atomic]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L4792-L4824
[pal-atomic]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L94-L126
[ring-publication]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1954-L2050
[gang-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L452-L458
[gang-commands]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L580-L611
[batch-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1827-L1903
[pal-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L175-L211
[pal-tracker]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L404-L479
[pal-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L628-L649
[pal-reuse]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L703-L739
[pal-increment-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2480-L2520
[pal-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L371-L449
[pal12-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L184-L213
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2645-L2679
[semaphore-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2939-L2973
[cik-emitter]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/cik_sdma.c#L217-L242
[cik-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_semaphore.c#L34-L106
[cik-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/cik_sdma.c#L565-L633
[cik-sync]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_sync.c#L121-L204
[radv-gang]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1376-L1457
[mesa-poll]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L36-L57
[fused-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1409-L1434
[fused-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2714-L2796
[fused-count]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L696-L755
[wide-controls]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2663-L2711

[hdp-setting]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L256-L260
[hdp-emission]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L548-L558
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L346-L384
