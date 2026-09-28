# Staged GPU-local memory

A staged transfer keeps CPU ingress and egress in system memory while shaders
operate on device-local VRAM or HBM. SDMA moves payloads between those
mappings; AQL packet fences establish the shader-side visibility edges. This
chapter combines the HSA copy and dispatch contracts with Linux's GC9.4.3/4
mapping policy and ROCr's actual dependency/cache sequence.

## Placement and scope

VRAM/HBM is a physical placement. SYSTEM in an AQL fence is a visibility
scope; it does not move that backing into host memory. A shader can write HBM,
release those writes to SYSTEM scope, and let SDMA read the same HBM
allocation. Conversely, an AQL SYSTEM acquire can make a completed SDMA upload
visible to shader loads of HBM.

ROCr's `hsa_amd_memory_async_copy` contract requires system-level coherent
buffers because DMA may lie outside the shader coherency domain. In general,
this entails sender SYSTEM release before copying and receiver SYSTEM acquire
before consuming the result. Both agents must have access to both buffers.
This is the basis for the conservative cache recipe, rather than an assumption
that SDMA shares the shader's caches. [ROCr copy contract][copy-contract]

HSA packet fences cover global memory. A dispatch acquire precedes active
execution, and its release finishes before the completion signal is
decremented. Packet fences cover the relevant dispatches across queues of the
same logical agent. BARRIER_AND waits for its dependencies and blocks later
packet processing through its completion phase. These semantics provide the
ordering points for the cross-engine flow. [HSA System Architecture 1.2,
§§2.9.1–2.9.2 and 3.3.8][hsa]

## Actor flow and ownership

The staged flow keeps upload/readback buffers, queue state, code, arguments
and control signals in coherently mapped system memory. Only input/output
payloads occupy HBM. The CPU neither maps nor accesses HBM through the device
aperture. Code publication completes separately before payload work.

| Actor | Work and ownership edge |
| --- | --- |
| CPU | Fill system-memory staging, publish those writes, and initialize dependency signals. Previous users have completed before storage or signals are reused. |
| Upload SDMA | Copy staging into device-local input and any output region whose prior contents must be preserved, then signal upload completion. |
| AQL | Wait for upload completion, acquire at SYSTEM scope, execute the compiled dispatch, release at SYSTEM scope, then decrement compute completion. |
| Download SDMA | Wait for compute completion, copy device-local output into system-memory readback, then report transfer completion. |
| CPU | Acquire-observe final transfer completion and read the result; join every independent user and retire command bytes before reusing the corresponding storage. |

The acquire must follow the upload dependency. A NONE/NONE dependency barrier
followed by a SYSTEM-acquire/SYSTEM-release dispatch provides that ordering,
as in ROCr's [dependency barrier][compute-dependency] and [compute
dispatch][compute-blit]. Kernargs remain immutable through dispatch
completion. Reusing HBM also requires every previous reader to finish and
every previous shader writer to release before SDMA overwrites the range. The
first upload to fresh storage has no prior shader user to retire.

SDMA completion and shader cache visibility are separate obligations. ROCr
emits its dependency polls before the copy and its completion update after the
transfer. It uses FENCE rather than an ordinary WRITE for the non-atomic
completion path because copy and write packets can overlap. That FENCE orders
the SDMA transfer; it does not replace the preceding shader SYSTEM release or
the following shader SYSTEM acquire. [ROCr completion][copy-completion],
[ordered submission][copy-submission]

Control words need their own coherent placement and signal protocol. A
one-writer 64-bit signal observed through its low DWORD can represent a 1-to-0
transition only while the high DWORD remains zero and the terminal zero stays
stable through every waiter. That protocol is narrower than a general 64-bit
timeline. Ring read-pointer progress permits command-storage reuse;
dispatch/transfer completion permits reuse of the resources those commands
reference. Neither observation substitutes for the other.

## Native HBM cache policy

KFD's VRAM allocation flag selects VRAM placement, except when the device's
`apu_prefer_gtt` policy redirects it to GTT. PUBLIC requests CPU access to the
VRAM allocation. COHERENT, EXT_COHERENT, and UNCACHED are separate flags,
translated independently into GEM flags. With none of those flags, VM mapping
starts with the default memory type and applies the BO and consumer's native
policy. [KFD mapping defaults][mapping-defaults] [Linux
allocation][allocation]

For native GC9.4.3/4, `gmc_v9_0_get_coherence_flags` chooses the following
mappings when UNCACHED and EXT_COHERENT are clear:

| Condition | Effective policy |
| --- | --- |
| VRAM owned by this device, with matching BO and VM memory-partition IDs | RW by default; module policy can select NC or CC. |
| VRAM outside that locality predicate | NC. Sharing a physical device does not alone establish partition locality. |
| Non-VRAM on the discrete device | UC; host cacheability and snooping have separate controls. |

The branch also sets the snoop bit. NC is non-coherent caching, not uncached
access. Linux's `mtype_local` values 1 and 2 select NC and CC respectively;
the default branch selects RW. Its per-page NUMA override applies to a
separate APU configuration. [PTE policy][pte-policy], [module
policy][mtype-policy], [APU override guard][apu-override]

LLVM's gfx942 memory model explicitly permits a logical agent with one or
multiple L2 caches. Its SYSTEM acquire sequence accounts for stale NC lines,
while RW/CC use the documented probe mechanisms; its SYSTEM release sequence
performs L2 writeback and waits for completion. The normal DMA contract and
full SYSTEM packet scopes therefore support one conservative recipe across
RW/NC/CC and partition configurations. The HSA scope expresses the obligation
for the logical agent; callers do not replace it with a guessed per-XCC
maintenance sequence. [LLVM topology and caching][llvm-model], [SYSTEM
acquire][llvm-acquire], [SYSTEM release][llvm-release]

MTYPE and partition geometry affect the route and cost of those operations.
[AGENT scope](../aql/barriers.md#fence-scope-and-observers) still covers an
execution agent with multiple L2s; that topology alone does not require
SYSTEM. Choosing a narrower scope requires the actual participating observers
and cache routes to lie within that scope. Shader instruction sequences
explain the memory model; they are not a published implementation of the AQL
firmware's cache operations.

Independent command implementations corroborate the required directions.
Linux's GC9.4.3 PM4 emitter combines cache writeback with completion and emits
global acquire operations. Mesa performs L2 writeback before signaling an SDMA
consumer, and PAL requires writeback/invalidation when crossing between shader
L2 and clients outside it. These are supporting implementation evidence, not
alternative AQL packet encodings. [Linux release][native-release], [Linux
acquire][native-acquire], [Mesa handoff][mesa-handoff], [PAL cache
transitions][pal-cache]

## HDP and the host-aperture boundary

HDP is the host data path used for legacy CPU access to VRAM through the PCIe
aperture. Linux orders CPU aperture writes before flushing the HDP write cache
and invalidates the HDP read cache before aperture reads where that operation
applies. APU and host-XGMI paths have different handling; some HDP revisions
omit the read-invalidate operation. HDP maintenance does not provide shader L2
release/acquire semantics. [Aperture routing][aperture-routing], [aperture
access][aperture-access], [flush exclusions][hdp-exclusions], [read-invalidate
versions][hdp-invalidate]

ROCr's ordinary gfx9 SDMA implementation uses its V4 variant without GCR. HDP
has a separate predicate: the runtime enables it by default, and on gfx942 the
SDMA object considers it supported when the link from the GPU to the first CPU
agent is not XGMI. The general predicate also excludes gfx10.1. This decision
is not based on the copy's allocation class or direction.
`HSA_ENABLE_SDMA_HDP_FLUSH=0` disables the runtime policy; the environment
switch is not evidence that omission is valid for a particular transfer.
[Variant selection][sdma-variant], [default][hdp-default], [link
predicate][hdp-link]

When enabled and supported, the ordinary ROCr path emits HDP after dependency
polls and before the submitted copy commands. Its user-queue operation is
distinct from the kernel-ring emitter, which supplies native register offsets
and engine masks. The latter corroborates HDP functionality without supplying
a user-queue encoding. [ROCr ordering][copy-submission], [user
emitter][hdp-user], [kernel emitter][hdp-kernel]

Using system-memory staging keeps the CPU out of the VRAM aperture. The
shader/SDMA cache handoff still exists, and ROCr's default implementation
still applies its broader HDP policy. The aperture's purpose alone does not
redefine that runtime's complete submission sequence. A transport that omits
an HDP operation must establish that no participating access uses the path it
protects.

## Architecture coordinates

Linux maps physical GC9.4.2 to compiler target gfx90a, while GC9.4.3 and
GC9.4.4 map to gfx942. SDMA IP is selected independently. Those coordinates
determine different packet, executable, and mapping choices. XCC and
memory-partition geometry additionally determine the native locality predicate
in the PTE table. [Native target translation][target-translation]
[Architecture coordinates](../architectures.md)

[copy-contract]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2132
[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[compute-dependency]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L682-L686
[compute-blit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L880-L911
[copy-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L477
[copy-submission]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L634
[mapping-defaults]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L513-L524
[allocation]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1712-L1820
[pte-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1046-L1199
[mtype-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c#L839-L843
[apu-override]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1330-L1342
[llvm-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11263-L11339
[llvm-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11560-L11585
[llvm-release]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L12492-L12535
[native-release]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L2985-L3017
[native-acquire]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L3496-L3512
[mesa-handoff]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1954-L1986
[pal-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Barrier.cpp#L1314-L1338
[aperture-routing]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1703-L1724
[aperture-access]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L827-L860
[hdp-exclusions]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L6541-L6561
[hdp-invalidate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v4_0.c#L39-L53
[sdma-variant]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L869
[hdp-default]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L231-L232
[hdp-link]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L187-L201
[hdp-user]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2986-L2989
[hdp-kernel]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L424-L436
[target-translation]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L341-L349
