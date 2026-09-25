# SDMA linear copy

COPY_LINEAR transfers an exact byte range between GPU virtual addresses.
ROCr's ordinary builder uses seven DWORDs: header, byte count minus one,
parameters, source low/high, and destination low/high. It accepts byte-aligned
ranges, including one-to-three-byte tails. The source and destination must not
overlap under the HSA async-copy contract. [Builder][rocr-copy]
[Layout][rocr-layout] [Overlap contract](ordering.md#overlap-and-ownership)

Pitched rows and slices use the separate
[rectangular-copy packet](rectangular-copy.md). Its element counts, geometry,
and per-generation scope fields have their own encoding and caller limits.

## Representation

| DWORD | ROCr ordinary linear-copy fields |
| --- | --- |
| 0 | Opcode 1 in bits 7:0, suboperation 0 in bits 15:8; NPD at bit 28 when the scoped template is selected. |
| 1 | Positive byte length minus one in the selected 22-bit or 30-bit count union. |
| 2 | Destination scope in bits 19:18 and source scope in bits 27:26. The scoped builder sets both to SYS (3); the unscoped builder leaves them zero. Temporal hints and other fields remain zero. |
| 3–4 | Full source byte address, low/high. |
| 5–6 | Full destination byte address, low/high. |

The builder zeroes the packet before setting these fields. NPD means “no prior
dependency” in ROCr's template description. Its selection and the distinction
from Linux's ordinary copy are described in
[ordering](ordering.md#scope-and-prior-dependency). The count and metadata
variations below identify other source-selected forms.

## Count representation and native IP

For a positive length `L`, an N-bit `L - 1` field represents `1..2^N` bytes.
Thus a 30-bit count can represent 1 GiB, even though its largest field value
is `0x3fffffff`. A zero field means one byte. Zero-length work omits the copy.
Mesa uses direct `L` before SDMA4.0 and `L - 1` from SDMA4.0, so old
direct-count limits cannot be reused without identifying the encoded quantity.
[Mesa emitter][mesa-copy], [ROCr fields][rocr-layout]

Linux selects backends using native SDMA IP and revision, independently of
compiler GFX names. [Native selection][linux-discovery]

| Native SDMA IP | Count and Linux ordinary-copy evidence |
| --- | --- |
| 4.4.2 | Raw `L - 1`, seven DWORDs and a `0x40000000`-byte cap. ROCr's gfx94x path independently selects a 30-bit union. The inherited Vega header still has a 22-bit count macro, which this emitter does not use. [Emitter/cap][linux442], [inherited macro][linux-vega-count], [ROCr selection][rocr-select] |
| 6.0.0–6.0.3, 6.1.0–6.1.4 | Native 30-bit count definition, `L - 1`, seven DWORDs and a `0x40000000`-byte cap in the shared v6 backend. [Header][linux6-count], [emitter/cap][linux6] |
| 7.0.0/7.0.1 | Uses the v6 30-bit count and `L - 1`, with a `0x40000000`-byte cap. Linux sets CPV and emits an eighth metadata DWORD. [Emitter/cap][linux7] |
| 7.1.0 | Its own 30-bit count definition, `L - 1` and a `0x40000000`-byte cap. The emitter writes seven DWORDs; its allocation accounting reserves eight. [Header][linux71-count], [emitter/cap][linux71] |

The count does not identify the whole packet. PAL's uncompressed GFX12 copy
omits the optional metadata DWORD that Linux's 7.0 path emits. These are
different source-selected forms, not interchangeable fixed lengths. [PAL
builder][pal12]

## Runtime caps and policy margins

| Consumer and selection | Maximum bytes in one ordinary copy |
| --- | --- |
| Linux backends in the table above; PAL's 30-bit builders | `0x40000000` (1 GiB). |
| ROCr's larger ISA-selected override | `0x3fffffff` (1 GiB minus one byte); largest emitted count is `0x3ffffffe`. |
| Mesa helper at native SDMA5.2 or later | `0x3fffff00` (1 GiB minus 256 bytes). |

ROCr selects the larger override for ISA major 9 with minor at least 4 or
exactly 9.0.10, major 10 with minor at least 3, and majors 11/12. Other
selected major-9/10 cases use `0x3fffff`. Disabling the override uses the
older `0x3fffe0` cap and 22-bit union. The ordinary builder emits successive
exact `min(remaining, cap)` ranges without DWORD rounding. The inspected code
gives no rationale for the override's one-byte headroom.
[Selector][rocr-select], [builder][rocr-copy], [fallback cap][rocr-layout]

Mesa uses `0x3fff00` below SDMA5.2. Its comment attributes that older cap to
an undocumented restriction near the top of the 22-bit count range, without an
affected-IP list, firmware threshold or mechanism. Relative to the older
direct-count maximum the margin is 255 bytes; relative to the modern
representable byte length it is 256. This describes a maximum-length policy,
not corruption of the last 255 bytes of arbitrary copies. The source does not
establish why later generations retain the margin. [Cap
definitions][mesa-caps], [helper selection][mesa-copy]

Mesa's 5.2 threshold is not a universal hardware width transition. Linux's v4
backend already selects the 1 GiB cap at native SDMA4.4.0. PAL selects 22
versus 30 bits using its GFX10.3-or-later predicate; that consumer predicate
does not classify CDNA by compiler-major number. A runtime policy, an encoded
limit and a native-IP contract remain separate facts. [Linux v4
selection][linux-v4], [PAL builder][pal10]

## Chunking and alignment

PAL's caller keeps the remaining length and both addresses in `gpusize`. Its
builder first caps the chunk, then rounds down to a DWORD multiple when both
addresses are DWORD-aligned and at least four bytes remain. The caller
advances by exactly the returned byte length; a final one-to-three-byte tail
uses a byte copy. Aligned 13-byte ranges therefore become 12 bytes plus one
byte, with no padding or overrun. The source explains that aligned addresses
and length let firmware use a faster DWORD mode. [Caller][pal-loop],
[builder][pal10]

Linux's TTM caller separately rounds the maximum chunk down to 256 bytes where
needed for memory-channel utilization and DWORD mode, assuming aligned
page-copy addresses. The 1 GiB cap already meets that alignment. This is a
performance policy, not corroboration of Mesa's undocumented margin. [TTM
chunking][linux-loop]

## Programming sequence and lifetime

ROCr's copy submission first satisfies dependencies and performs its selected
cache acquire, then executes the copy and any required cache release before
updating completion. PAL and RADV insert a [pending-transfer
drain](ordering.md#pending-transfer-drains) before a downstream copy reads the
preceding copy's output. The [cache chapter](cache.md) identifies which native
owner supplies each operation; the packet's byte count says nothing about that
responsibility.

Source storage remains unchanged through its final read. Destination storage
remains live through every downstream reader, and the completion cell through
its final writer and waiter. Primary-ring consumption permits ring-byte reuse;
it does not replace the completion of referenced transfers. [Native copy
stream](atomics.md#copy-completion-through-add64)

[linux-discovery]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2836
[linux442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L2282-L2328
[linux-vega-count]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L116-L121
[linux-v4]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L2611-L2637
[linux6-count]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L149-L154
[linux6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1821-L1863
[linux7]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1753-L1813
[linux71-count]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L149-L154
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1716-L1759
[linux-loop]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2493-L2564
[mesa-caps]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L356-L360
[mesa-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L92-L126
[rocr-select]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L903
[rocr-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2140-L2184
[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L84-L150
[pal10]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L453-L523
[pal12]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L388-L453
[pal-loop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L447-L480
