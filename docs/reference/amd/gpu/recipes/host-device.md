# CPU and GPU memory handoff

A CPU produces input, a GPU consumes it and writes a result, and the CPU
consumes that result. Each transfer needs both an execution dependency and
visibility through the consumer's memory path. Mapping the same allocation
into both address spaces establishes access; it does not establish either
handoff. Completion also has an owner: a shader's completion can retire its
arguments while a later copy or another queue still borrows its output.

The direct flow below uses host-accessible, fine-grained HSA memory and an
ordinary AQL dispatch. Native KFD/GEM mappings, PM4 barriers, CPU apertures and
staged transfers supply different implementations of the surrounding memory
edges. Their allocation and transport premises remain explicit.

## Mapping properties describe different things

| Property | What it determines |
| --- | --- |
| Physical placement | System DRAM, device VRAM/HBM, or backing that the native memory manager may migrate. |
| CPU mapping | Whether the CPU can address the allocation and whether accesses use cached, write-combining or uncached treatment. |
| GPU mapping | Access permissions, address translation, snooping and memory type, including whether GPU caches participate. |
| Observer scope | Which agents a release/acquire operation makes writes visible to; SYSTEM does not mean system-memory placement. |
| Control protocol | How a consumer recognizes the relevant completed producer and how the control value remains observable. |

Linux supplies a concrete example of the first three properties being
independent. KFD's owned GTT allocation selects the GTT domain; COHERENT,
EXT_COHERENT and UNCACHED separately become GEM flags. USERPTR registration is
a different allocation path. TTM chooses cached CPU backing unless
CPU_GTT_USWC requests write-combining. For GTT, it adds SYSTEM PTE treatment
and adds SNOOPED when the backing is cached. In the GMC11 mapping path,
COHERENT, EXT_COHERENT or UNCACHED selects GPU MTYPE_UC. A CPU-cached mapping
can therefore coexist with a GPU-uncached mapping. [KFD allocation][kfd-alloc]
[CPU cache selection][ttm-cache] [PTE attributes][ttm-pte]
[GMC11 memory type][gmc11]

The GEM COHERENT flag itself says that GPU instructions may still be needed
to flush caches at SYSTEM scope. UNCACHED describes use of GPU caches, not
the CPU mapping. Neither flag means that outstanding shader work has
completed. GC9.4.3/4 have a different PTE policy, including partition-sensitive
VRAM locality and UC non-VRAM mappings on the discrete device; the complete
predicates are in [staged GPU-local memory](local-memory.md#native-hbm-cache-policy).
[GEM flag contract][gem-flags] [GC9 policy][gmc9]

These native mappings are examples, not a translation of every HSA pool
allocation into one KFD allocation kind. ROCr reports pool grain, location and
per-agent accessibility separately; its system-region policy sets
`CachePolicy=CACHED` and independently sets `Uncached` for kernarg regions.
The latter does not replace the CPU cache-policy selection. A caller
uses the reported pool and access contract rather than inferring it from a
CPU pointer, a compiler target name or physical DRAM placement.
[ROCr region construction][region-flags] [Pool properties][pool-properties]
[Per-agent access][pool-access]

## A complete fine-grained AQL flow

The participants in this flow are the host and one selected HSA GPU agent.
Input and output reside in host-accessible global memory whose pool reports
FINE_GRAINED. Kernargs use a region or pool that permits kernarg
initialization. The executable has already been loaded, made visible to
instruction fetch, and retained by its owner. A native HSA signal represents
completion; it is not an arbitrary payload word cast to a signal handle.
[Pool grain flags][pool-flags] [Signal representation][signal-create]
[Executable publication](../aql/dispatch.md#executable-publication-and-final-use)

1. Query the pool's allocation allowance, granule and alignment, and the
   selected GPU's access to it. Allocate input, output and kernarg backing.
   Where access must be enabled, `hsa_amd_agents_allow_access` names every
   agent that must retain direct access; the pool's owning agent remains
   included. The call is not an additive grant to an otherwise unknown set.
   [Allocation contract][pool-allocate] [Access update][allow-access]
2. Establish exclusive write ownership for this use. Fill input and arguments
   and initialize output as needed. Create a completion signal with value 1
   and a consumer set that includes its host waiter, or with the unrestricted
   consumer form. Keep the arguments unchanged until dispatch completion.
   [Signal consumers][signal-create] [HSA §3.3.3.1][hsa]
3. Prepare a dispatch packet with SYSTEM acquire and SYSTEM release. Reserve
   a writable ring slot, write its body while INVALID, then release-publish
   its valid first DWORD and notify the queue. Host writes must be published
   for their actual mappings and store instructions before that transition.
   [AQL publication](../aql/publication.md#packet-publication)
4. The packet processor performs the dispatch acquire before active shader
   execution. The shader reads input and writes output. The completion phase
   performs the selected release and finishes it before atomically
   decrementing the completion signal. [HSA §§2.9.1–2.9.2][hsa]
5. The host waits with `hsa_signal_wait_scacquire` and inspects the returned
   value. A returned zero establishes this single-dispatch completion; merely
   returning from the wait does not. The API permits a spurious return, and
   the condition must stay satisfied until all dependent waiters have
   observed it. Only after the successful acquire observation does the host
   read output. [Acquire-wait contract][signal-wait]
6. Before the next use, join every additional reader, writer and signal
   consumer. Then rearm the completion value and rewrite the corresponding
   payload or arguments. Reusing a ring slot additionally requires its
   read-index retirement. Freeing code, backing or the queue requires the
   final users of those owners to have ended, not just one convenient
   completion observation. [Packet and task lifetime](../aql/publication.md#completion-and-queue-lifetime)

There are two distinct input-visibility rules in this sequence. HSA requires
kernarg contents to be released to SYSTEM before dispatch, which the release
publication of the valid packet format can provide. The packet processor
makes those kernargs visible without requiring an acquire fence in the
packet *for kernarg loads*. The payload's global-memory acquire is a separate
edge. Publishing arguments is not a substitute for acquiring input data or
publishing executable code. [HSA §3.3.3.1][hsa]

HSA signal operations have the specified memory order at SYSTEM scope. That
property belongs to the signal operation; it does not grant the same atomic
capability to arbitrary payload addresses. Nor does a host/GPU fine-grained
allocation establish coherence for an additional DMA device or an agent that
has not been granted access. [HSA §2.5][hsa] [Access update][allow-access]

ROCr's ordinary compute blit is a real consumer of this ordering. It places
dependency barriers before a SYSTEM-acquire/SYSTEM-release dispatch,
release-publishes the packet header, and uses the dispatch signal for its
blocking completion wait. The blocking path checks the acquired result for
zero before returning success. This is runtime policy built on the HSA
contract, not a claim that every transfer is executed by a shader.
[Dependency construction][blit-deps] [Dispatch construction][blit-dispatch]
[Blocking completion][blit-wait]

## CPU publication and device cache operations

A CPU publication operation orders the CPU's writes. A device acquire makes
the relevant device loads observe them. GPU uncached control storage can make
a marker observable without supplying the cache operations required for a
separately cached payload. Conversely, a payload invalidate placed after a
control wait cannot rescue a wait that keeps reading a stale control value.
The [cross-queue handoff](../pm4/handoff.md) makes that split explicit for PM4.

Host store ordering follows the actual CPU mapping and instruction sequence.
For example, ROCr's blit producer issues `SFENCE` when publishing to a device
ring on its selected PCIe-ordering path, before the atomic release header
store. Its native x86 doorbell path also has explicit store ordering. These
operations order CPU stores; they do not invalidate shader caches or wait for
a shader to finish. A plain release operation, a write-combining drain and a
GPU cache command have different responsibilities.
[Blit publication][blit-publish]
[Ring and doorbell stores](../aql/publication.md#ring-placement-and-x86-stores)

PAL's GFX9-family barrier implementation gives the cache direction a concrete
representation. It classifies CPU/memory accesses as bypassing GL2. A
transition from those clients to a GL2 client requests GL2 invalidate plus
writeback, with destination shader-cache invalidation where required. The
opposite transition requests GL2 writeback. Its planning also accounts for
execution stages; cache maintenance alone is not the execution join.
[Access classes][pal-clients] [Cache directions][pal-cache]
[PM4 execution and cache ordering](../pm4/cache.md)

For a raw PM4 flow, the CPU-visible completion must be downstream of the
relevant shader join and release, through control storage suitable for that
write and host observation. An ordinary CP store after a dispatch does not
alone establish shader completion. PAL illustrates the distinction by using
an EOP release for compute/bottom-of-pipe events and WRITE_DATA
for CP-stage events. A host acquire orders later CPU loads after the observed
marker; it does not retroactively flush the producer's dirty GPU cache lines.
[Stage-selected event emission][pal-event]

The native transport owns any additional submission trailer and completion
object. Scheduled DRM work and a direct user queue need not carry the same
trailer. Their [command-buffer completion](../pm4/command-buffers.md) and
[cache protocols](../pm4/cache.md) remain part of the flow even when payload
and control use the same physical allocation kind.

## Mapping is not an execution handoff

PAL permits an allocation to remain CPU-mapped while GPU command buffers
reference it. Its Map/Unmap pair establishes or removes CPU access; it does
not assign simultaneous write ownership. [PAL mapping contract][pal-map]

RADV similarly maps a BO through the native GEM mmap interface and unmaps the
CPU view separately. Its advertised host-visible types include both cached
GTT and write-combined GTT, and both are HOST_COHERENT. Its mapped-memory
flush/invalidate entry points return success without cache work. That is an
implementation choice paired with those memory types, not a rule that every
Linux GPU mapping has coherent CPU caches. RADV's optional device-coherent
types separately request GL2_BYPASS, translated to GPU MTYPE_UC on GFX9 and
newer. [RADV memory types][radv-types] [CPU mapping][radv-map]
[Mapped-range operations][radv-ranges] [Device-coherent types][radv-coherent]
[GPU mapping][radv-pte]

Ordinary `hsa_amd_memory_lock` is another distinct contract. It pins an
existing host allocation and returns an agent address that can differ from
the host address. Overlapping locks can produce aliases that are not
necessarily coherent with each other, and the ordinary returned agent
pointer has coarse-grained access. Pinning therefore does not turn an
arbitrary host allocation into the fine-grained flow above. Unlock uses the
original host pointer while that allocation is still alive.
[Lock contract][memory-lock] [Unlock contract][memory-unlock]

ROCr's synchronous `Runtime::CopyMemory` handles this ownership explicitly:
it checks pointer ownership, locks a host range when needed for the selected
GPU, substitutes the returned GPU address, and keeps the lock through the
copy call before unlocking. Agent mapping and transfer completion are
different owners even when the temporary lock is hidden inside a synchronous
operation. [ROCr host-copy owner][runtime-copy]

## Local apertures and staged transfers

CPU-visible VRAM is an aperture path, not the same mapping as CPU-cached
system DRAM. Linux's native aperture-access helper orders its CPU writes
before an HDP flush; for reads it invalidates HDP before reading where that
operation applies. HDP maintenance covers this host path and does not replace
shader-cache release/acquire. [Aperture access][aperture]

The architecture and transport predicates matter. Linux bypasses its ordinary
HDP flush and invalidate paths for an APU outside passthrough under
`CONFIG_X86_64`. Its CPU-connected XGMI exclusion is separate and is not
guarded by that CPU architecture check. HDP 4.4.0, 4.4.2 and 4.4.5 omit the
cited read-invalidate operation. These are driver-selected cases, not a general
inference from the words APU, coherent or Large BAR. [HDP exclusions][hdp-exclusions]
[Read-invalidate revisions][hdp-invalidate]

System-memory staging avoids CPU access through that aperture. The complete
flow is CPU publication of upload data → transfer completion → shader
acquire/work/release → readback transfer completion → host acquire and read.
The upload buffer remains borrowed until upload completes; the local output
remains borrowed through readback. [Staged GPU-local memory](local-memory.md)

ROCr's asynchronous-copy API requires system-level coherent source and
destination buffers and access by both specified agents. Its fence guidance
is explicitly “in general”: sender SYSTEM release before copying and
receiver SYSTEM acquire before consumption. Dependency signals reach zero
before copying; the completion signal is decremented afterward, and a
negative value reports an error. A source signal observed by a later copy
must therefore remain alive and stable through that consumer's dependency
processing. [Asynchronous-copy contract][async-copy]

The ordinary SDMA implementation puts dependency polls before its copy and
the completion update after it. When platform atomics are unavailable it
uses FENCE instead of WRITE, because ordinary copy and write packets can
overlap. Its HDP policy is separate: for ISA major at least 9, excluding
gfx10.1, support depends on a non-XGMI link to the first CPU agent, and the
enabled path emits HDP after dependency polls. That broader runtime policy
does not disappear merely because one particular copy uses staging.
[Completion choice][sdma-completion] [Submission order][sdma-submit]
[HDP predicate][sdma-hdp]

## Imported buffers and final use

Linux DMA-BUF supplies an explicit CPU cache-access boundary for mapped
imports: SYNC_START with the appropriate READ/WRITE direction precedes CPU
access, and SYNC_END follows it. These operations provide cache coherency,
not exclusion against concurrent devices. The application waits for prior
device use before beginning CPU access and publishes new device use after
ending it, using the importer's actual fence/dependency protocol.
[DMA-BUF CPU-access contract][dmabuf-cpu]

The exporter can perform substantial work at that boundary. AMDGPU's
begin-CPU-access implementation may move an unpinned, eligible BO to GTT
for CPU reads. The import path preserves selected coherence/cache flags
when the exporter uses AMDGPU's own operations; that is not a promise about
arbitrary exporters. Sharing a handle and addressability alone therefore
cannot extend the host/GPU coherent recipe to another device.
[CPU-access implementation][dmabuf-begin] [Import flag boundary][dmabuf-import]

| Storage | Final use that must end before rewrite, rearm or release |
| --- | --- |
| Input payload | Every shader or transfer still reading that version. |
| Output or intermediate payload | Its producer release and every downstream reader; host readback also needs the final host acquire. |
| Kernargs and executable | Every dispatch that can fetch them, including their required backing and prefetch extents. |
| Completion/dependency signal | Its producer update and every host/device waiter; a stable terminal value is part of the protocol. |
| Ring slots and indirect command storage | The command processor's retirement boundary, separately from resource completion. |
| CPU mapping, agent mapping and allocation | All users of that address or backing, followed by the owning native unmap/unlock/free operations. |

The single-dispatch sequence has one terminal result. A multi-queue or staged
graph may need several joins to close these lifetimes. A completed producer
does not revoke a consumer's borrow, and rearming a shared signal too early
can erase the very completion that consumer still needs to observe.
[AQL dependency lifetime](../aql/barriers.md)
[PM4 handoff ownership](../pm4/handoff.md#owners-and-a-complete-sequence)

[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[kfd-alloc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1712-L1783
[ttm-cache]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1190-L1214
[ttm-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1432-L1476
[gmc11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c#L468-L513
[gem-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L158-L175
[gmc9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1102-L1159
[region-flags]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_memory_region.cpp#L68-L114
[pool-properties]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_memory_region.cpp#L309-L352
[pool-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_memory_region.cpp#L355-L401
[pool-flags]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1830-L1857
[signal-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L1382-L1437
[pool-allocate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2035-L2088
[allow-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2673-L2711
[signal-wait]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2042-L2086
[blit-deps]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L676-L706
[blit-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L887-L916
[blit-wait]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L624-L648
[blit-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L911-L929
[pal-clients]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L56-L68
[pal-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L324-L365
[pal-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1327-L1383
[pal-map]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palGpuMemory.h#L604-L635
[radv-types]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L531-L572
[radv-map]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c#L708-L774
[radv-ranges]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device_memory.c#L346-L400
[radv-coherent]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L628-L648
[radv-pte]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c#L33-L48
[memory-lock]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2785-L2821
[memory-unlock]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2872-L2888
[runtime-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L594-L662
[aperture]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L827-L860
[hdp-exclusions]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L6541-L6575
[hdp-invalidate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v4_0.c#L39-L53
[async-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2132
[sdma-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[sdma-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L634
[sdma-hdp]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L187-L202
[dmabuf-cpu]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/dma-buf.h#L26-L85
[dmabuf-begin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c#L280-L319
[dmabuf-import]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c#L415-L446
