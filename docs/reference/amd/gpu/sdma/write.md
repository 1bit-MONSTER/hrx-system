# SDMA inline data writes

`WRITE_LINEAR`, named `SDMA_PKT_WRITE_UNTILED` in the packet definitions,
copies DWORD values carried inside the command stream to a writable GPU
address. It has no separate source allocation. The command storage supplies
both the operation and its input data; the destination and any later consumer
have their own completion and lifetime requirements. [KFD builder][builder]

KFD's ordinary USER-queue caller submits this packet to mapped process memory.
Linux's native SDMA4.4.2 and SDMA6.x ring checks independently emit a single
DWORD write. These are distinct submission owners using the same unscoped
packet shape. [USER caller][caller] [SDMA4.4.2 caller][linux442]
[SDMA6.x caller][linux6]

## Representation

The modern count-minus-one form consists of four header DWORDs followed by
exactly `N` data DWORDs. The destination and byte length are DWORD-aligned.
KFD's builder selects this count convention at `FAMILY_AI` and later; its
older path encodes the count directly. PAL's builder agrees on the modern
encoding. [KFD builder][builder] [PAL builder][pal-builder]
[Alignment precondition][pal-update]

| DWORD | Fields and units |
| --- | --- |
| 0 | Opcode 2 in bits 7:0; subopcode 0 in bits 15:8. The cited ordinary builders leave the other header fields zero. |
| 1–2 | Low and high halves of the absolute GPU destination byte address. |
| 3 | Bits 19:0 contain `N - 1`, measured in DWORDs. Other fields depend on the native layout below. |
| 4 through `N + 3` | The `N` DWORD values, in destination order. |

Count zero therefore writes one DWORD. Empty host work emits no packet; it
cannot be represented by subtracting one from zero. The 20-bit count represents
up to `2^20` data DWORDs, but that is not a command-buffer or ring-capacity
promise. The producer must reserve space for all four header words and all
inline data before publication. [Count definition][count]
[USER publication](publication.md)

For example, writing three values uses seven DWORDs: `2`, the two address
halves, `2`, then the three values. The next command starts immediately after
the third value. Inline data is not a pointer or an indirect command stream.

## Native layout and caller differences

The low 20-bit count agrees across the inspected definitions. Upper fields
do not have one generation-independent interpretation:

| Source-selected layout | Upper fields of DWORD 3 |
| --- | --- |
| PAL GFX10/GFX11 | Swap field at bits 25:24; its GFX10.3+ view names bits 28:26 `cache_policy`. The ordinary inline builder leaves them zero. |
| PAL GFX12 | SYS at bit 20, SNP at bit 22, GPA at bit 23, and `dst_mall_policy` at bits 29:28. |
| KFD `FAMILY_GFX125X` and later | The USER builder sets `scope` at bits 27:26 to SYS (3); its other optional fields remain zero. |

[PAL GFX10 fields][pal-fields] [PAL GFX12 fields][pal12-fields]
[KFD fields][fields] [KFD system scope][scope] [KFD selection][builder]

KFD allocates zeroed packet storage before assigning its selected fields, so
the other optional fields are zero rather than inherited command bytes.
[Packet storage][allocation]

The pinned Linux SDMA7.1 header still labels bits 28:26 `cache_policy`.
That definition does not corroborate the KFD scoped interpretation of bits
27:26. The active native layout and producer predicate must distinguish these
uses; sharing an opcode and count field does not make the policy operands
interchangeable. [Linux SDMA7.1 fields][linux71-fields]

An API named “update memory” also need not emit this packet. PAL's
`BuildUpdateMemoryPacket` is called for image compression metadata, whereas
its ordinary `CmdUpdateMemory` places caller data in embedded GPU storage and
emits copies from that storage. These paths have different command sizes and
source-storage lifetimes. [Metadata caller][pal-metadata]
[Update-memory caller][pal-update]

## Ordering, completion and ownership

A WRITE is transfer work. ROCr explicitly uses FENCE instead of WRITE for
completion because serial copy/write packets can overlap. Observing a later
WRITE is therefore not a general completion certificate for earlier work.
[Completion choice][completion] [Transfer ordering](ordering.md)

A command stream updating data for a subsequent SDMA copy can express the
dependency as follows:

```text
selected dependency and cache acquire
WRITE_LINEAR values -> A
NOP                            complete pending transfers before reading A
COPY_LINEAR A -> B
selected cache release
FENCE completion               publish completed payload to its observer
```

The NOP supplies the transfer dependency, while mapping attributes and cache
operations supply visibility. The final FENCE orders the work and writes a
separate completion word. [Ordering boundary](ordering.md)
[Cache operations](cache.md) [Completion store](fence.md)

The host's original array can be reused after its values have been copied into
command storage. The published command bytes remain immutable through native
command consumption; any submission owner can impose a longer command-storage
lifetime. The destination remains live through its final reader, including
the dependent copy in this sequence. Completion storage remains live through
its final writer and observer. KFD's caller checks the destination separately
from ring consumption before destroying its queue, illustrating that command
retirement and payload observation are distinct. [Builder][builder]
[Caller][caller] [Queue publication and consumption][publication]

[builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/SDMAPacket.cpp#L30-L70
[fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/sdma_pkt_struct.h#L146-L215
[scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/sdma_pkt_struct.h#L37-L43
[allocation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BasePacket.cpp#L49-L60
[caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDQMTest.cpp#L195-L213
[publication]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/SDMAQueue.cpp#L69-L98
[completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[linux442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1056-L1098
[linux6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L903-L953
[count]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1584-L1604
[linux71-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3115-L3136
[pal-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1033-L1057
[pal-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L3452-L3520
[pal12-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L3476-L3535
[pal-metadata]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1385-L1413
[pal-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1062-L1117
