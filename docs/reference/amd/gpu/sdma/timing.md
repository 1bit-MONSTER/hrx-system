# SDMA timestamps

GET_GLOBAL records the GPU global timestamp in memory. PAL documents that the
operation waits for preceding commands, so the timestamp is also an ordering
point for transfer work. It measures a position in the SDMA stream rather than
the duration of a shader or the host's submission call. [PAL timestamp][pal]

## Representation

The ordinary packet occupies three DWORDs: opcode 13 and suboperation 2 in the
header, then the destination byte address low/high. ROCr clears the packet and
sets SYS scope at header bits 25:24 only for its scoped template. Mesa emits
the same baseline header and address. PAL additionally selects cache policies
when MALL is supported. [ROCr definition][layout] [ROCr builder][builder]
[Mesa emitter][mesa] [PAL timestamp][pal]

ROCr's signal owner records a 32-byte alignment requirement for gfx7/gfx8 and
an eight-byte requirement for gfx9. It places both SDMA timestamp fields at
32-byte-aligned offsets, preserving one storage layout across those paths. The
copy-profiling caller also checks that alignment before the ending sample.
[Signal storage][placement] [Profiling caller][stream]

## Programming sequence

A stream can record a start sample, execute a transfer, and record an end
sample. The ending timestamp waits for the preceding transfer. A final
completion update gives the observer a separate readiness event for the
timestamp destinations and payload. The timestamp and completion storage stay
live through their final writes and reads. ROCr places profiling timestamps
inside its dependency/cache/completion envelope; the precise positions define
which work the measured interval includes. [ROCr stream][stream]

Converting the difference to elapsed time requires the native clock frequency.
Comparing it with CPU or another device's time requires clock correlation and
its uncertainty. Raw samples alone do not provide either input. The [GPU
observation chapter](../observability.md) describes those native clock queries
and separates global timestamps, shader counters, and performance counter
collection.

[pal]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L107-L137
[layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L940-L968
[builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2961-L2977
[mesa]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L25-L33
[placement]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/signal.h#L146-L225
[stream]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L540-L586
