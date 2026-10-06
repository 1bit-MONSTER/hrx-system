# SDMA DWORD constant fill

The ordinary five-DWORD CONST_FILL packet repeats a 32-bit pattern over a
nonempty, DWORD-aligned destination range. [PAL's caller][pal-caller] requires
four-byte alignment for both address and length. [PAL's builder][pal-builder]
and [Mesa's independent emitter][mesa-builder] select fillsize 2 and a byte
count minus one. [ROCr's public fill API][rocr-api] takes a count of uint32
elements; its [packet builder][rocr-builder] converts that count into a
byte-derived field. API elements and packet count units are distinct.

## Representation

PAL, Mesa, and ROCr use the following unscoped DWORD-fill form. Each builder
starts from zero or fills every emitted word; optional cache/scope fields
retain the source-specific meanings described below.

| DWORD | Selected fields and units |
| --- | --- |
| 0 | Opcode 11 at bits 7:0, subopcode 0, fillsize 2 at bits 31:30; header `0x8000000b` when optional policy/scope fields are zero. |
| 1–2 | Low/high halves of the real writable GPU destination address. |
| 3 | Full 32-bit repeating pattern. |
| 4 | `byte_length - 1`; length is positive and divisible by four. In DWORD mode the low two encoded count bits are ignored. |

Ignoring those low bits makes `byte_length - 1` equivalent to ROCr's
`byte_length - 4`. The effective DWORD count is `(encoded_count >> 2) + 1`.
Zero-length host work omits the packet; a zero count field in ROCr's spelling
means one DWORD, not zero work.

## Count limits and generation differences

| Source | Observed mode and software bound |
| --- | --- |
| PAL GFX10/11 builder | Mode 2; 22-bit/30-bit count by its generation predicate, capped at `2^22 - 4` / `2^30 - 4` bytes. |
| Mesa | Mode 2; native SDMA below 6 uses the conservative 22-bit bound, SDMA 6+ the 30-bit bound, with the same four-byte margin. |
| ROCr | Mode 2; shared 22-bit structure and a `0x3fffe0`-byte software split limit. |
| Linux exact SDMA4.4.2 | Mode 0 and a 1 GiB software maximum, despite an inherited header with a 22-bit count definition. |
| Linux SDMA6.0 backend | Mode 0 and a 1 GiB software maximum with a 30-bit count definition. |

The [exact 4.4.2][linux442] and [6.0][linux6] Linux emitters use mode zero;
their larger byte-fill limits do not resolve the different mode-two upper
bounds chosen by the user runtimes.

PAL additionally sets the generation-specific cache-policy and CPV fields when
the device reports MALL support. ROCr's scoped fill sets SYS in header bits
25:24 and NPD at bit 29. Those positions belong to CONST_FILL, not
COPY_LINEAR. Swap, byte and halfword fill modes, cache policies, and newer
metadata layouts have their own representations. [Scoped
builder][rocr-builder] [Fill layout][rocr-layout]

## Execution, completion and lifetime

A native fill sequence establishes writable mappings for the destination,
performs the selected transport's dependency and cache acquire operations,
emits one or more CONST_FILL packets, then performs cache release and
publishes completion. ROCr routes its fill through the same submission
envelope as its copy path, including the selected HDP/USER_GCR operations and
signal update. [Fill submission][rocr-submit] [Blocking owner][rocr-blocking]
[Cache composition](cache.md)

Independent disjoint fills may overlap. Reusing a filled range in a dependent
transfer requires the [ordering boundary](ordering.md). A plain WRITE after
CONST_FILL is not the completion operation used by ROCr: its submission uses
FENCE or atomic signal completion. The target allocation remains live until
the fill and any later consumers have completed; completion storage has its
own last writer and waiter. [Completion protocol](atomics.md)

[pal-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1196-L1231
[pal-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1171-L1227
[mesa-builder]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L70-L90
[rocr-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2890-L2913
[rocr-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2605-L2637
[linux442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L2299-L2328
[linux6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1838-L1867
[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L561-L610
[rocr-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1933-L1947
[rocr-blocking]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L364-L394
