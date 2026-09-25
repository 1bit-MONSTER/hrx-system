# PM4 shader and SDMA handoffs

A shader/SDMA handoff needs separate contracts for payload visibility,
progress-word visibility and final resource ownership. RADV supplies a real
source example: a transfer command buffer alternates SDMA image copies with
compute-shader fallbacks for transfers SDMA cannot perform. The native owner
submits the two streams as a DRM gang of indirect buffers. [Memory/image
caller][m-copy], [image/image caller][m-copy-image]

## Execution and payload visibility

Application dependencies feed RADV's gang barrier state. That state carries
directional progress values, shader cache operations and stage completion;
merely recording commands in two streams does not establish their ordering.
The actual fallback caller then emits the required edge:

| Direction | Producer and progress | Consumer and payload visibility |
| --- | --- | --- |
| SDMA → compute | Transfer, then SDMA FENCE to a directional control DWORD. | CP full-mask GE wait, shader cache maintenance including `INV_L2`, then dispatch. |
| Compute → SDMA | BOTTOM_OF_PIPE_TS release to the other control DWORD with immediate32 data and write confirmation. Transfer queues add GL2 writeback on GFX10+, or the older TC actions on earlier generations. | SDMA full-mask GE poll, then transfer. This path emits no inline SDMA USER_GCR. |

[Dependency lowering][m-barrier], [progress writes][m-progress], [wait
selection][m-wait], [actual copy flow][m-copy]

On GFX10+, RADV's `INV_L2` lowers to GL2 writeback **and** invalidation;
before GFX12 it also includes metadata maintenance. Destination-access flags
supply the relevant scalar/vector cache invalidations. An L2 action alone is
not a complete shader acquire. PAL independently preserves the same
directions: GL2 producer to bypass consumer needs writeback; the reverse
transition needs invalidation plus writeback to preserve other dirty data.
[RADV cache lowering][m-cache], [shader access][m-access], [PAL
actors][p-actors], [transition planner][p-cache], [GFX12 planner][p-cache12]

The progress allocation is distinct from the payload. For a noncoherent gang,
RADV allocates two directional DWORDs in a separate zeroed VRAM BO with
`GL2_BYPASS`; its VA mapping translates that property to UC on GFX9+. A device
may also select separate VRAM control storage for placement reasons. Neither
policy requires making all payload memory uncached. [Control
allocation][m-control], [VA mapping][m-map], [GFX11 PTE mapping][l-pte]

The coherence helper returns true for a graphics-CP leader with a compute
follower. For an SDMA leader it returns true only through GFX8 or when the
exact-GFX12 system-memory-scope property is set. Its noncoherent branch
therefore includes GFX9, despite a nearby comment naming only GFX10–11. On
GFX12, CP/SDMA sharing a view still leaves shader GL2 outside that view. The
pinned RADV revision rejects generations beyond GFX12. [Exact
helper][m-control], [device property][m-property], [RADV device
selection][m-admission], [GFX12 cache route](../pm4/dma.md)

## Final join and replay

RADV has a second, queue-owned control allocation for the gang preambles and
postambles. It always requests `GL2_BYPASS`; on GFX9+ this selects UC mapping.
The first wait can precede normal preamble cache maintenance. The compute
postamble writes a confirmed BOP completion, and the leader postamble waits
for it before the leader finishes. This joins final work independently of the
within-command-buffer progress milestones. [Queue join][m-queue-control]

Linux makes the need concrete: the leader depends on follower **scheduled**
fences, while the returned submission fence follows the leader's **finished**
fence. Scheduling the follower does not mean it has finished using command or
payload storage. The explicit terminal join supplies that missing edge.
[Kernel gang submission][l-gang]

For replay, the command-buffer finalize path has each consumer reset the
opposite directional semaphore. Reuse still requires completion of the
previous execution, and both engines' last uses belong to the final owner.
These finite 32-bit values do not establish a wrap-safe general timeline.
[Command-buffer finalize][m-finalize], [terminal command-buffer work][m-end]

## Native transport and memory mappings

Mesa disables SDMA direct user queues at this source revision. Its DRM IB
submission includes native engine identity, BOs and synchronization; kernel
submission/cache services are distinct from commands appended to a persistent
KFD ring. The [SDMA cache reference](../sdma/cache.md) separates those
transports and the USER_GCR/GCR_REQ forms. [User-queue selection][m-userq],
[DRM submission][m-transport]

KFD's COHERENT allocation flag becomes GEM_COHERENT. The GFX11 PTE builder
selects UC for COHERENT, EXT_COHERENT, or UNCACHED BOs. Without CPU_GTT_USWC,
TTM uses cached CPU pages and marks their GPU mappings SYSTEM+SNOOPED. These
native conditions explain how GPU-uncached access and CPU write-back access
can coexist; CPU cacheability by itself does not determine the GPU route.
[Flag translation][l-kfd-flags] [CPU mapping][l-ttm] [SYSTEM/SNOOPED
mapping][l-snooped] [PTE policy][l-pte]

PAL distinguishes UC requests from its default read-no-allocation/write-bypass
policy for cacheable SDMA requests. A no-allocation policy alone does not rule
out a stale cache hit. The payload mapping and the progress mapping therefore
remain explicit parts of a handoff. [SDMA cache policy][p-sdma-policy]

Cross-device attachment also preserves native ownership: KFD's attachment path
uses the original BO or an imported DMA-BUF for the target device, and the
importer preserves the relevant BO flags. Each consumer still derives its own
PTE/cache behavior and needs a reachable address. Sharing an allocation does
not by itself establish an ordering edge or a mutually atomic control cell.
[KFD attachment][l-attach] [BO flag preservation][l-import]

## Programming sequence

RADV's complete transfer/compute composition has these boundaries:

1. Allocate the directional progress cells and map all payload/command storage
   in the native submission's address space.
2. Record the SDMA transfer and its directional FENCE. The compute stream waits
   on that value before its shader cache acquire and dispatch.
3. Record the compute release and confirmed progress store. SDMA polls that
   value before reading the shader-produced payload.
4. Finalize the command buffers, recording each consumer's zero write to its
   consumed directional cell for replay.
5. Append the queue-owned terminal join so the returned submission fence covers
   the follower's final use as well as the leader's final use.
6. After that completion, reuse the command storage or replay the recorded
   protocol.

This sequence composes the cited progress, cache, and terminal-join owners. A
direct KFD ring has its own publication and retirement protocol and receives
none of the DRM wrapper's cache or residency work implicitly.

[m-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L106-L155
[m-copy-image]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L632-L682
[m-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1774-L1829
[m-progress]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1954-L2037
[m-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.h#L172-L184
[m-cache]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L74-L112
[m-access]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L7992-L8077
[p-actors]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L56-L68
[p-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L347-L365
[p-cache12]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L408-L433
[m-control]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1832-L1940
[m-map]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c#L33-L50
[l-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c#L468-L513
[m-property]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1214-L1218
[m-admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2502-L2514
[m-queue-control]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1346-L1492
[l-gang]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L1282-L1339
[m-finalize]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L2059-L2085
[m-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L8876-L8893
[m-userq]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1499-L1504
[m-transport]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1718-L1772
[l-kfd-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1774-L1779
[l-ttm]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1190-L1214
[l-snooped]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1432-L1476
[l-attach]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L917-L955
[l-import]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c#L415-L446
[p-sdma-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L415-L449
