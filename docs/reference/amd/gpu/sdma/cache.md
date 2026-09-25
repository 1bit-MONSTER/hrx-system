# SDMA cache maintenance and transport

The normal ROCr user-queue copy path and Linux's kernel-ring submission path
use distinct cache packets. Their similar control operands do not establish
interchangeable queue use. The packet form, scope and surrounding operations
follow the native queue's ownership and privilege model.

## Ordinary user-queue composition

ROCr's V5 blit implementation creates a normal KFD SDMA user queue and emits
dependency polls, applicable HDP maintenance, USER_GCR acquire, the copy body,
USER_GCR release and completion. It then publishes the finished ring extent
through the write pointer and doorbell. Cache commands sit inside the
caller-owned queue; no scheduled kernel IB wrapper runs for each such copy.
[Queue creation][rocr-init], [stream construction][rocr-submit],
[completion][rocr-completion], [publication][rocr-publish]

The classic USER_GCR is five DWORDs with opcode 17 and suboperation 1. Its
19-bit control occupies DWORD 2 bits 31:16 and DWORD 3 bits 2:0. The builder
clears the packet before setting these operations:

| Position in the copy stream | ROCr's selected control |
| --- | --- |
| Acquire before copy | `0xc3c0`: GL2/GLK writeback and GL2/GL1/GLV/GLK invalidation. |
| Release after copy | `0x8040`: GL2/GLK writeback. |

Base, limit, range selectors, sequence and other operands remain zero. In
particular, the builder's comment about discarding lines does not set the
distinct GL2_DISCARD field. These are the complete masks emitted by that
builder. [Fields][rocr-fields] [Builder][rocr-gcr]

ROCr's factory selects the composition using ISA and transport, rather than
discovered SDMA IP:

| Source predicate | Selected behavior |
| --- | --- |
| Major 9 | V4: no GCR or per-copy scope fields; applicable HDP work remains separate. |
| Major 10, or major 11/12 with minor below 5; non-DXG | V5: USER_GCR acquire/release. |
| Major 11/12 with minor at least 5; non-DXG | V6: per-packet scope fields, no GCR. |
| DXG on the major-10/11/12 paths | V4; the source attributes GCR wrapping to its underlying driver. |

Initialization excludes HSA full-profile agents. The V6 comment describes
OSS7.1/DACC, which alone does not explain the broader major-11.5 factory
predicate. For non-DXG gfx11.5 the factory selects V6 without USER_GCR. The
DXG wrapping statement belongs to that driver transport.
[Factory][rocr-select] [Variant definitions][rocr-variants]

Each packet builder still owns the scope-field encoding. ROCr's
[rectangular-copy branch](rectangular-copy.md#rectangular-specific-scope-boundary)
sets those fields only in its GFX12-or-later layout, so its gfx11.5 rectangular
path does not inherit the ordinary linear-copy scope writes.

## Kernel submission and generation differences

PAL's Linux `Queue::AddIb` requests `AMDGPU_IB_FLAG_EMIT_MEM_SYNC` on the
first IB. Linux's scheduled submission wrapper consumes that request before
the IB, invoking the engine's kernel GCR emitter; it separately handles HDP.
On SDMA6 that emitter uses suboperation 0 and an explicit VMID-zero operand.
This kernel-owned operation does not establish the packet a persistent KFD
user ring should emit. Mesa also excludes SDMA from its direct-userq mask at
the pinned revision. [PAL request][pal-submit], [kernel
wrapper][linux-submit], [kernel packet][linux-gcr], [Mesa
selection][mesa-userq]

PAL's GFX12 definitions independently name USER_GCR suboperation 1 and a
distinct user packet whose upper half of DWORD 4 is reserved. The request
packet has a VMID field there. ROCr's reuse of a struct containing a VMID
member, left zero by its user builder, does not establish an explicit user
VMID selection contract. Firmware context inheritance and permitted range
operations require their own specification. PAL's GFX12 layout also has a
20-bit control and broadcast field; the classic 19-bit form has a different
control representation. [PAL suboperation][pal-subop] [Distinct
layouts][pal-user]

Linux SDMA7.1 uses a six-DWORD layout with wider addresses and relocated
controls. ROCr contains a corresponding gfx1250 builder branch, but its normal
factory selects V6 with GCR disabled there. A present builder branch is not
evidence of an active ordinary caller. [Linux layout][linux71], [ROCr
builder][rocr-gcr], [factory][rocr-select]

## HDP and command publication

ROCr places applicable HDP maintenance after dependency waits and before GCR
and payload work. Its support predicate includes major 9 and later except
10.1, and excludes a GPU-to-CPU XGMI link. That predicate, the selected backing
and transport remain part of the recipe. In-stream HDP or GCR cannot publish
the command bytes needed to fetch that same command; ring backing and host
publication ordering have their own contract. [Support][rocr-init],
[stream order][rocr-submit]

The HDP enable setting is sampled at initialization and checked at stream
construction in addition to the platform support predicate. System-memory
placement, CPU access to a VRAM aperture, peer routes, and GPU cache policy
are distinct inputs to a visibility sequence. An omitted cache packet in one
runtime template does not describe every one of those access paths.
[Setting][rocr-hdp-setting] [Emission][rocr-hdp-emission] [Device-local and
system-memory paths](../recipes/local-memory.md)

[rocr-init]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L156-L242
[rocr-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L516-L578
[rocr-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L650
[rocr-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1997-L2032
[rocr-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1002-L1059
[rocr-gcr]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2992-L3026
[rocr-select]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L884
[rocr-variants]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
[pal-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L1938-L1983
[linux-submit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L231-L317
[linux-gcr]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L293-L315
[mesa-userq]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1499-L1504
[pal-subop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L71
[pal-user]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2410-L2526
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L289-L310
[rocr-hdp-setting]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L256-L260
[rocr-hdp-emission]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L548-L558
