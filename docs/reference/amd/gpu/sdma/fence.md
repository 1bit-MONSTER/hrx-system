# SDMA completion stores

FENCE publishes a value after preceding transfer work. ROCr uses this
operation for completion because ordinary COPY and WRITE commands can overlap.
Its classic form writes one DWORD; atomic signal updates and the newer 64-bit
fence are separate operations. [Completion choice][choice]

## Representation and applicability

The classic packet has four DWORDs: opcode 5 in the header, destination byte
address low/high, and a 32-bit value. The destination is DWORD aligned.
Linux's SDMA4.4.2 emitter uses an opcode-only header. ROCr selects additional
fields by ISA major and by its template parameters. [Linux
emitter][linux-fence] [ROCr builder][builder] [ROCr layouts][layouts]

| ROCr predicate | Fields set by the FENCE32 builder |
| --- | --- |
| ISA major < 10 | Opcode 5; other header fields zero. |
| ISA major 10 or 11 | Classic MTYPE bits 18:16 set to 3. |
| ISA major >= 12 | New two-bit MTYPE at bits 17:16 set to 3, SYS at bit 20 set to one because ROCr uses this fence for system-memory signals. |
| ISA major >= 12 and `scopeFields` | Additionally, SYS scope (3) at bits 25:24. |

The builder's ISA-major branch precedes its scope decision. Selecting the V6
scoped template for gfx11.5 therefore does not add a scope field to its
FENCE32, even though the template sets scope fields on other operations. The
[factory predicates](atomics.md#architecture-and-transport-differences)
identify which template ROCr selects for each native transport.

PAL's GFX12 FENCE union reserves header bits 19:16 and 25:24 where ROCr names
MTYPE and scope. PAL instead names MALL policy at bits 27:26. That source
disagreement remains unresolved by the shared opcode; the definitions describe
different source-selected field interpretations. [PAL layout][pal-layout]

## Execution and lifetime

A completion sequence consists of payload work, the payload's required cache
release, and the completion update. A consumer acquires the control word under
its mapping's visibility contract before accessing the payload. Cache actions
for the payload and visibility of the control word are independent; a later
payload acquire cannot repair an unreadable completion predicate. [Native
submission sequence](atomics.md#copy-completion-through-add64) [Cache
ownership](cache.md)

One FENCE32 is not an indivisible 64-bit signal operation. ROCr's non-atomic
completion path computes the new signal value on the CPU and emits a
conditional high-word fence followed by the low-word fence. Linux's scheduled
64-bit fence sequence instead writes low then high before TRAP. Each sequence
belongs to its own observer and lifetime protocol. [ROCr
completion][completion] [Linux emitter][linux-fence]

ROCr may append a mailbox FENCE and TRAP after its output signal update. The
output value can therefore be visible while notification commands still use
the signal's mailbox. Payload completion, notification completion,
primary-ring consumption, and the final use of each allocation are distinct
observations. [Notification owner](atomics.md#memory-and-lifetime)

## Native 64-bit form

ROCr's gfx1250 FENCE_64B is opcode 5/suboperation 2 and occupies five DWORDs.
It writes a 64-bit value to an eight-byte-aligned address. The builder selects
MTYPE 3, SYS, and optional SYS scope. This is a separate representation from
two FENCE32 packets; the corresponding [atomic
chapter](atomics.md#gfx125-fused-waitcopysignal) describes the selected
copy/signal protocols. [64-bit layout][wide-layout] [64-bit
builder][wide-builder]

[choice]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2097-L2137
[layouts]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L612-L690
[linux-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L450-L476
[pal-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2274-L2293
[completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L663
[wide-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L692-L742
[wide-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2689-L2711
