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

## Pool grain, agent access and SVM

Pool grain describes the memory-consistency contract for accesses to an
allocation. Direct access is a separate relationship between that pool and
each participating agent. HSA exposes these facts through different queries:

| Attribute | Query inputs | Meaning |
| --- | --- | --- |
| `HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS` | Global memory pool. | Grain and whether allocations permit kernarg initialization. |
| `HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS` | Requesting agent and memory pool. | Whether that agent can directly access allocations, and whether access requires an explicit grant. |
| `HSA_AMD_AGENT_INFO_SVM_DIRECT_HOST_ACCESS` | GPU agent. | Whether the host can directly access SVM memory physically resident in that GPU's local memory. |

[Pool flags][pool-flags] [Global-pool attribute][pool-global-attribute]
[Agent/pool attribute][pool-access-attribute] [SVM attribute][svm-host-access]

The access enumeration has three answers, all with the
`HSA_AMD_MEMORY_POOL_ACCESS_` prefix:

| Value | Answer | Direct-access contract |
| --- | --- | --- |
| 0 | `NEVER_ALLOWED` | The agent cannot directly access buffers in this pool; an access grant cannot establish this path. |
| 1 | `ALLOWED_BY_DEFAULT` | The agent can access buffers without an explicit grant. |
| 2 | `DISALLOWED_BY_DEFAULT` | The agent needs a successful `hsa_amd_agents_allow_access` call for the buffer before direct access. |

An access-set update includes every agent that must retain direct access;
the pool owner remains included. The pool-level
`HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL` reports whether every agent can
be granted access, not which particular pairs require grants or deny access.
[Access values][pool-access-values] [Access update][allow-access]
[Aggregate accessibility][pool-accessible-by-all]

The pinned ROCr `MemoryRegion::GetAccessInfo` applies the following ordered
policy. Later rows apply only when an earlier row did not return:

| Condition | Returned access |
| --- | --- |
| The requesting agent owns the pool. | `ALLOWED_BY_DEFAULT` |
| The link has fewer than one hop. | `NEVER_ALLOWED` |
| System pool, requesting CPU. | `ALLOWED_BY_DEFAULT` |
| System pool, other requesting agent. | `DISALLOWED_BY_DEFAULT` |
| Local pool with `fine_grain() == false`, including coarse and extended-scope fine grain. | `DISALLOWED_BY_DEFAULT` |
| Ordinary fine-grained local pool with equal requesting/owning `HiveId()` values. | `DISALLOWED_BY_DEFAULT` |
| Remaining cases. | `NEVER_ALLOWED` |

The hive comparison has no separate nonzero check. Pool exposure is another
predicate: ROCr makes ordinary fine-grained local pools user-visible when the
GPU's `HiveID` is nonzero or `HSA_FORCE_FINE_GRAIN_PCIE` is exactly `1`.
Exposure does not replace the per-agent query. Separately, ROCr answers the
SVM attribute from KFD's `CoherentHostAccess` capability; that query receives
neither a pool nor a requesting CPU agent. [Access implementation][pool-access]
[Pool exposure][pool-exposure] [Option parsing][fine-grain-pcie]
[SVM capability implementation][svm-host-capability]

The public header's access-attribute comment disagrees with these narrower
contracts: it says fine-grained pools cannot return `NEVER_ALLOWED`, and
requires an explicit grant for any pool not associated with the agent. The
access enumeration explicitly permits access without a grant for
`ALLOWED_BY_DEFAULT`; the implementation returns that value for CPUs accessing
system pools and can deny ordinary fine-grained local memory to another agent.
The returned agent/pool access value distinguishes these cases. Fine grain,
an exposed GPU pool or the SVM capability alone cannot establish the complete
CPU/GPU allocation contract. [Attribute comment][pool-access-attribute]
[Access values][pool-access-values] [Access implementation][pool-access]

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
   host and selected GPU agents' access to it. Allocate input, output and
   kernarg backing.
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

## Resident CPU/GPU exchange

A dispatch acquire precedes entry into the active phase. Host writes
published later while the shader remains resident need a new synchronization
edge inside that shader. An HSA exchange uses signal operations or admitted
global-memory atomics whose scope covers both agents, with payload backing
that obeys that memory model. An HSA signal's SYSTEM-scope semantics belong
to its native signal object; an arbitrary payload word has its own atomicity
and mapping contract. [HSA §§2.5 and 3.3.8][hsa]

For one input slot, the CPU finishes payload writes and release-publishes
its ready generation. The shader acquires that generation before reading
payload, then release-publishes input credit after every shader reader has
finished. The CPU acquires that credit before rewriting the slot. For an
output slot, the shader joins its writers and release-publishes the result;
the CPU acquires and reads it, then release-publishes credit. The shader
acquires that credit before the next overwrite. This composes the two
directed handoffs into a reusable slot; the [pipeline
recipe](../../interop/pipelines.md#resident-execution-and-slot-generations)
also covers generation arithmetic, multiple readers and native drain.

The shader acquire is more than a fresh control load. LLVM's GFX10/GFX11
GLOBAL atomic acquire-load mapping at AGENT/SYSTEM scope uses `glc=1`
(`dlc=1` additionally on GFX10), waits for the load with `s_waitcnt vmcnt(0)`,
then invalidates GL1 and GL0 before subsequent payload loads. This sequence
assumes the target's admitted atomic access and L2 coherence or bypass route.
Invalidation after a polling loop cannot repair a loop that never observes
the control update. [Acquire sequence][shader-acquire]
[Hierarchy and mapping premises][shader-model]

The ordinary scalar-load path assumes its data remains unchanged during the
dispatch. Lane-uniform addresses do not give mutable control or payload that
property. Immutable kernarg pointer values can point to separately synchronized
mutable data. A publishing wave's wait counts also cover its own accesses;
other contributing waves or workgroups require a join. LLVM's release sequence
orders preceding loads as well as stores, which matters when the publication
returns credit for completed reads. [Scalar-memory premise][shader-model]
[Release sequence][shader-release]

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

The copy API excludes dependencies on future async-copy submissions because
native queue placement can deadlock them. A visible control value and valid
wait encoding do not establish that their producer can run. The completion
value also has a different lifetime from later notification commands: ROCr
can place a mailbox FENCE and TRAP after the completion update. Its gang-copy
leader polls each participant signal before performing that signal's final
update, so its storage cannot be destroyed during the leader's last read.
[Copy dependency restriction][async-copy]
[Completion and notification][sdma-notification]
[Gang-signal final use][sdma-gang]

## Peer GPU handoff

A second GPU adds another agent, mapping and directed access path. HSA AGENT
scope covers one agent; synchronization with another agent requires the wider
matching scope. One physical package need not be one agent, and one GFX942
agent can contain several L2 caches. Scope therefore does not translate into
a count of packages or XCCs. LLVM's GFX942 memory model and the native
GC9.4.x memory mappings retain separate local/remote cache treatment.
[HSA scope instances][hsa] [GFX942 hierarchy][gfx942-model]
[Native memory policy](local-memory.md#native-hbm-cache-policy)

ROCr exposes pool access, cache-coherent links and 32/64-bit atomic link
properties separately. The access-set update lists all permitted agents plus
the pool owner. For its asynchronous copy, both named agents must directly
access both buffers at their current locations, and payloads must satisfy the
API's system-coherence contract. These are different premises from the
existence of an interconnect or shared handle. [Pool/link properties][peer-access]
[Access update][allow-access] [Copy contract][async-copy]

A direct peer flow releases the source, establishes the dependency, then
acquires at the receiver before reading peer payload. A copy-based flow
instead releases the source, satisfies the transfer dependency, copies and
reports completion; the receiver acquires before using the destination. The
source stays borrowed through every peer read, and the destination through
its consumers. The native queue's engine must also serve the directed route.
ROCr separates topology-based blit selection, destination-specific recommended
engines, and explicit-engine restrictions. Its preferred-engine masks have
target-specific CPU/GPU choices, while targets without dedicated xGMI engines
use a different selection rule. The [SDMA engine-selection
chapter](../sdma/engine-selection.md) traces those predicates and their native
queue classes. These routing rules supply no additional memory-scope guarantee.

The native attachment also determines whether peer access retains device-local
placement. AMDGPU's DMA-BUF path couples P2P eligibility to exporter power
ownership and placement, while KFD can share same-hive backing directly.
The [external-memory placement contract](../../interop/external-memory.md#peer-placement-and-exporter-power)
distinguishes those paths from engine selection and payload synchronization.

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
[pool-global-attribute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1871-L1888
[pool-access-attribute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2606-L2630
[pool-access-values]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2503-L2523
[pool-accessible-by-all]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1922-L1928
[pool-exposure]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L532-L569
[fine-grain-pcie]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L237-L238
[svm-host-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L821-L826
[svm-host-capability]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2584-L2587
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
[shader-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L13564-L13695
[shader-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L13906-L13932
[shader-release]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L14367-L14414
[sdma-notification]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L649
[sdma-gang]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L589-L612
[gfx942-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11263-L11333
[peer-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2580-L2647
