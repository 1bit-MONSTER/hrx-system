# Shader memory publication

A resident shader transfers ownership of commands or payloads by completing
its accesses, publishing a control value, and acquiring the next owner's
result. Shader wait counters, cache instructions and memory-instruction policy
fields implement different parts of that sequence. The acquire and release
around a whole dispatch cannot synchronize exchanges that occur while the
dispatch remains active.

This chapter describes ordinary global-memory publication across GCN, CDNA
and RDNA. LLVM's pinned instruction mappings provide the per-family sequences;
its emitter and Mesa's access-policy lowering expose the conditions behind
them. Native mapping, external atomic access and another engine's completion
protocol remain separate inputs. The [CPU/GPU](recipes/host-device.md),
[GPU/NPU](recipes/gpu-npu.md) and [device-generated SDMA](sdma/device-publication.md)
recipes supply those surrounding owners.

The GFX125x compiler rows include `gfx1250`, identified as CDNA5 by ROCm's
[target registry](architectures.md#cdna5-and-gfx1250). CDNA1–4 sequences retain
their individual GFX9 target predicates.

## Ordering, visibility and scope

A handoff has three distinct memory obligations:

| Obligation | Meaning |
| --- | --- |
| Producer release | Earlier payload accesses finish with the ordering and write visibility required by the receiving participant before control is published. |
| Fresh control observation | The consumer's control load can observe the publication through its actual mapping and cache path. |
| Consumer acquire | After observing the publication, subsequent payload accesses see the released data and cannot move before the acquire. |

A release orders prior **loads as well as stores**. Returning input credit
after consuming a slot therefore needs read completion; draining stores alone
does not establish that the producer can overwrite the slot. A shader acquire
similarly does more than load a fresh flag: other cached payload locations may
still need invalidation. LLVM records both the compiler ordering constraints
and the machine operations separately. [Ordering constraints][llvm-order]
[Release construction][llvm-release-builder] [Acquire construction][llvm-load-builder]

The participating scopes, storage address spaces and native memory types all
matter. An agent can contain multiple L2 caches on gfx942 and GFX125x. An SDMA
engine on the same device can sit outside the shader coherence domain; ROCr's
copy contract consequently calls for SYSTEM-scoped sender release and receiver
acquire. Mesa independently selects system/memory scope for
`ACCESS_CP_GE_COHERENT_AMD` accesses when `gfx_level == GFX12`, while ordinary
coherent shader accesses select device scope. This is a source-specific access
policy, including CP/SDMA/GE clients, rather than a consequence of the engines
sharing a device identifier. [gfx942 topology][llvm-gfx942-model]
[GFX125x topology][llvm-gfx125-model] [DMA contract][copy-contract]
[Mesa cache-policy selection][mesa-cache-flags]

System memory describes placement; SYSTEM describes a synchronization scope.
On GFX10/GFX11, the documented sequences assume L2 is coherent with the other
agent or that the native mapping supplies the required bypass route. `glc`
and `dlc` on a control load do not manufacture that route. The
[native mapping discussion](sdma/device-publication.md#command-memory-and-pre-wptr-visibility)
shows why even a BO flag named `COHERENT` has generation-specific consequences.
[L2 premise and load policy][llvm-gfx10-model] [Emitter premise][llvm-gfx10-load]

## Wait counters and participating waves

The counters belong to the issuing wave. Their retirement reports completion
for the instruction classes they track; it does not join other waves or
determine the external visibility of every memory type.

| Shader family | Relevant completion classes |
| --- | --- |
| GFX6–GFX9, gfx90a, gfx942, gfx950 | VMEM uses `vmcnt`; LDS/scalar classes share `lgkmcnt`. Generic/flat accesses can require both classes when they can address LDS. |
| GFX10/GFX11, including gfx1150/gfx1151 | `vmcnt` tracks loads, returning atomics and samples; `vscnt` tracks stores and non-returning atomics. LDS/scalar operations use `lgkmcnt`. |
| GFX12.0 | Separate `loadcnt`, `storecnt`, `samplecnt`, `bvhcnt`, `dscnt` and `kmcnt` classes. A returning atomic contributes to the load side, a non-returning atomic to the store side. |
| GFX125x | Ordinary global load/store and LDS classes remain distinct. ASYNC LDS and tensor operations have `asynccnt` and `tensorcnt`; the pinned compiler model does not insert those waits automatically. `xcnt` tracks address translation. |

[Legacy and flat ordering][llvm-legacy-model] [RDNA counter model][llvm-gfx10-model]
[GFX12 counters][llvm-gfx12-model] [GFX125x asynchronous classes][llvm-gfx125-order]
[GFX12 emitter][llvm-gfx12-waits]

CDNA5's §5.7 distinguishes `ASYNCcnt` and `TENSORcnt` from ordinary load,
store and LDS completion. Its XML defines `S_WAIT_ASYNCCNT` and
`S_WAIT_TENSORCNT`; `GLOBAL_INV` uses the load counter, while `GLOBAL_WB`
and `GLOBAL_WBINV` use the store counter. [Architecture counters][cdna5-counters]
[Machine-readable instruction definitions][cdna5-xml]

[Asynchronous global/LDS transfers](async-memory.md) describes those two
classes, descriptor-generated notification and LDS split barriers. Transfer
readiness and the final reader's permission to reuse a slot form distinct
edges, including when the transfer and consumer run in different waves.

The RDNA4 ISA guide §5.7 and Table 26 distinguish in-order retirement within
a class from memory data ordering, and describe the scope at which store
completion can be reported. The [MEM_ORDERED discussion](pm4/dispatch.md#shader-wait-counter-mode-mem_ordered)
also separates register hazards from synchronization. Reaching a nonzero wait
threshold is useful only with the target's documented counter ordering and
the compiler's accounting of every intervening instruction.

Completion is also distinct from same-wave issue ordering. Ordinary vector
loads, stores and atomics from one wave retain program order when they access
the same address, so an overlapping later VMEM access does not by itself need
the earlier access to retire first. The compiler must still preserve their
issue-order dependency. Register-result readiness, source-register reuse,
program exit, volatile accesses that order different addresses, generic/flat
accesses that can reach LDS, and cross-wave synchronization retain their own
completion requirements. Loom applies this rule only to dependency links that
retain both endpoint instructions; a conservative CFG frontier does not retain
enough producer identity to prove the nonvolatile case. [RDNA4 vector-memory
ordering][rdna4-vector-order] [LLVM alias-wait insertion][llvm-alias-waits]

For a workgroup with several contributing waves, each wave participates in
the required release, the group rendezvous joins those contributions, and the
designated publisher performs the outward release before publishing control.
Each reader likewise needs an acquire path for its own accesses. A leader's
cache operation or wait is not an implicit join of all contributors.
[Execution-barrier memory rules][llvm-barrier-model]

The execution mode changes workgroup coherence. GFX10/GFX11 and GFX12.0 WGP
mode can place waves on different CUs with separate vector L0 caches; CU mode
keeps them on one CU. LLVM still waits on workgroup **release** in CU mode so
another wave can relay that data outward at a wider scope. GFX125x instead has
a shared WGP cache with two independently ordered request ports: sharing that
cache removes an invalidate, but does not remove the required waits between
waves. gfx90a/gfx942 `TgSplit` similarly changes the legacy same-CU assumption
and excludes LDS allocation. [CU/WGP release handling][llvm-gfx10-waits]
[GFX12/GFX125x workgroup handling][llvm-gfx12-workgroup]
[TgSplit handling][llvm-tgsplit]

A workgroup barrier has no cross-workgroup execution effect. A resident queue
whose progress depends on other workgroups also needs the independent
[scheduling and forward-progress contract](../interop/pipelines.md#progress-and-backpressure);
cache correctness alone does not supply it.

## Global release and acquire sequences

The following table summarizes LLVM's default availability/visibility mapping
for an ordinary global atomic control store/load at AGENT or SYSTEM scope.
The payload consists of global accesses, the scalar path contains only
dispatch-immutable data, and all control widths and mappings satisfy their
native atomic-access contract. The release column precedes the control store;
the acquire column begins with the control load. Arrows express ordering, not
a requirement to emit every wait as a separate instruction: a compiler can
combine or move waits while preserving these edges.

| Family | Release and control publication | Control observation and payload acquisition |
| --- | --- | --- |
| GFX6–GFX9, excluding the CDNA rows below | `s_waitcnt vmcnt(0)` → control store. SYSTEM visibility relies on the documented native coherence/bypass premise. | Control load with `glc=1` → `s_waitcnt vmcnt(0)` → vector L1 invalidate. LLVM uses `buffer_wbinvl1_vol` where its target/environment permits, otherwise `buffer_wbinvl1`. |
| gfx90a | AGENT: VMEM wait → store. SYSTEM: `buffer_wbl2` → VMEM wait → store. | AGENT: `glc` load → VMEM wait → L1 invalidate. SYSTEM additionally performs `buffer_invl2` before the L1 invalidate. |
| gfx942 and the inherited gfx950 compiler path | `buffer_wbl2` at the selected SC scope → VMEM wait → control store at that scope. | Control load at the selected SC scope → VMEM wait → `buffer_inv` at that scope. |
| GFX10/GFX11, including gfx115x | Wait for both `vmcnt(0)` and `vscnt(0)` → control store. | `glc` load, also `dlc` on GFX10 → `vmcnt(0)` → `buffer_gl1_inv` → `buffer_gl0_inv`. |
| GFX12.0 | SYSTEM adds `global_wb scope:SCOPE_SYS`; AGENT omits it. Wait for prior global loads and stores, including writeback completion → control store at the selected scope. | Load at the selected scope → `s_wait_loadcnt 0` → `global_inv` at that scope. |
| GFX125x | Wait for prior store/atomic effects to reach L2 → `global_wb` at AGENT/SYSTEM scope → wait for writeback and prior accesses → atomic control store at that scope, with its required translation wait. | Load at the selected scope → `s_wait_loadcnt 0` → `global_inv` at that scope → another `s_wait_loadcnt 0` before dependent accesses. |

[Legacy cache control][llvm-legacy-acquire] [Legacy/CDNA writeback][llvm-cdna-wb]
[gfx942 control policy][llvm-cdna-load-store] [GFX10/GFX11 acquire order][llvm-gfx10-acquire]
[GFX12 release mapping][llvm-gfx12-release] [GFX12 acquire mapping][llvm-gfx12-acquire]
[GFX125x release mapping][llvm-gfx125-release] [GFX125x acquire mapping][llvm-gfx125-acquire]

These classes follow the pinned emitter's generation dispatch and feature
inheritance: gfx950 includes the gfx94x feature set; gfx115x includes the
GFX11 feature set. That establishes the compiler paths, without asserting
identical native mapping or partition topology for those targets.
[Cache-control dispatch][llvm-cache-dispatch] [gfx950 inheritance][llvm-cdna-features]
[gfx115x inheritance][llvm-rdna-features]

### CDNA scope bits and cache locality

For gfx942 global loads, stores and cache-control instructions, the detailed
LLVM rows use `sc0=1, sc1=0` for workgroup, `sc0=0, sc1=1` for agent, and
`sc0=1, sc1=1` for system scope. Read-modify-write instructions interpret `sc0`
differently: it selects whether the old value is returned; `sc1` controls
their agent/system coherence behavior. The load/store table consequently
cannot be copied onto an RMW encoding. [SC load/store policy][llvm-cdna-load-store]
[RMW policy][llvm-cdna-rmw]

The pinned gfx942 hierarchy prose names `buffer_wbl2 sc1` for cross-agent
writeback. Its detailed SYSTEM-release row and the actual emitter instead
use **both `sc0` and `sc1`**; the table above follows those two precise sources.
Its AGENT-release row uses only `sc1`. This difference matters when selecting
the writeback domain. [Hierarchy wording][llvm-gfx942-model]
[Detailed AGENT/SYSTEM rows][llvm-gfx942-release] [Emitted fields][llvm-cdna-wb]

An L2-local RW/CC mapping and a nonlocal NC mapping have different probe and
invalidate behavior. With multiple L2 caches in one gfx942 agent, an
AGENT-scope operation can require L2 maintenance. With one L2, some such work
becomes a hardware no-op. The scope expresses the observer relation; counting
OS device handles does not replace the locality and partition facts.
[gfx942 hierarchy and memory types][llvm-gfx942-model]
[Native HBM mapping](recipes/local-memory.md#native-hbm-cache-policy)

### Vector cache scopes and special waits

On GFX10/GFX11, the acquire invalidates GL1 **before** GL0. LLVM's emitter
names the reason: otherwise GL0 can refill from stale GL1 data. Neither
operation is an L2 invalidate. The selected native L2 route remains necessary.
[Invalidate ordering][llvm-gfx10-acquire]

GFX12 instructions use `SCOPE_CU`, `SCOPE_SE`, `SCOPE_DEV` and `SCOPE_SYS` to
control how far an access must travel before a cache can service it or
acknowledge a store. `global_inv` targets caches below its selected scope;
`global_wb` pushes earlier lower-scope effects outward. For GFX12.0, LLVM's
AGENT release omits writeback; GFX125x adds device-scope writeback because
an agent can contain multiple L2s. On GFX12.0 `SCOPE_CU` denotes a CU,
independent of CU/WGP execution mode; on GFX125x it denotes the WGP.
[GFX12 scope model][llvm-gfx12-model] [GFX125x scope model][llvm-gfx125-order]
[Writeback selection][llvm-gfx12-wb]

CDNA5 Table 13 lists no L2 action for DEV-scoped maintenance. The same
manual's §4.1.1 makes cache scope memory-pool-dependent; LLVM's GFX125x model
also admits noncoherent L2s within one agent. The table alone cannot justify
eliding an AGENT writeback across those mappings. The unresolved input is the
deployed cache-scope assignment, not the numerical scope encoding.
[Manual scope rules and table][cdna5-scopes] [Compiler mapping model][llvm-gfx125-order]

The detailed GFX125x sequences require waits before `global_wb` so prior
store/RMW data has reached L2, and after agent/system `global_inv` so later
loads cannot overtake it. LLVM selects both through
`hasINVWBL2WaitCntRequirement()`. Its general hierarchy description alone
does not express these extra waits; the detailed rows and emitter agree on
them. A returning RMW belongs to the load completion class even when its
side effect must be included in a release.
[Target predicate][llvm-gfx125-cache-predicate]
[Pre-writeback waits][llvm-gfx12-wb] [Post-invalidate waits][llvm-gfx12-inv]
[Detailed release/acquire rows][llvm-gfx125-release] [Acquire row][llvm-gfx125-acquire]

Two further compiler-selected sequences have different purposes:

| Predicate and operation | Sequence and purpose |
| --- | --- |
| GFX12.0 `FeatureWaitsBeforeSystemScopeStores`, non-atomic store with SYSTEM instruction scope | LLVM waits on load, store and scalar `kmcnt`, plus sample/BVH when the target has image instructions, before the store. Its atomic-store path is separate. |
| GFX125x `requiresWaitXCntForSingleAccessInstructions()`, VMEM atomic store/RMW or non-atomic volatile access | LLVM emits `s_wait_xcnt 0` before the access so address-translation replay cannot repeat an operation that must execute once. This is separate from publishing the payload. |

[GFX12 feature selection][llvm-gfx12-store-feature]
[Special store/volatile handling][llvm-special-accesses]
[Translation-wait predicate][llvm-xcnt-predicate] [Pre-store wait classes][llvm-system-store-waits]

A shader using sample/BVH operations adds their completion classes to a
release. A fence joining LDS with global memory adds `lgkmcnt(0)` on the
older families or `dscnt(0)` on GFX12. An address-space-specific contract can
omit irrelevant waits; a generic pointer or a fence covering several spaces
does not carry that proof. GFX125x asynchronous LDS/tensor work has its own
explicit completion path. [Address-space fence rules][llvm-fence-spaces]
[Wait selection][llvm-gfx12-waits] [Asynchronous operations][llvm-gfx125-order]

## Scalar loads and per-access cache policy

LLVM's ordinary scalar-memory path assumes the data is unchanged for the
duration of the dispatch. A uniform pointer to mutable control or payload
does not satisfy that premise. An immutable kernarg can hold such a pointer,
but the pointed-to contents still need the mutable-data protocol. The scalar
and vector caches are not generally coherent with one another; dispatch-boundary
invalidation cannot repair a resident scalar poll. Legacy scalar spill stores
have their own compiler-managed `s_dcache_wb` lifetime and are not a model
for inter-agent publication. [Scalar premises][llvm-gfx10-model]
[Scalar spills][llvm-legacy-scalar]

CDNA5 §4.1.1 also excludes scalar/vector coherence at WGP scope. Its `NV`
cache-line policy is separate from language-level volatility: maintenance
with `NV=0` leaves nonvolatile lines intact; `NV=1` includes all lines.
[Scalar scope and NV policy][cdna5-scopes]
LLVM documents the same selection for GFX125x cache instructions while keeping
ordinary scalar loads restricted to dispatch-immutable data.
[Compiler NV and scalar contracts][llvm-gfx125-nv]

LLVM's availability/visibility model separates propagation of an individual
access from synchronization of other accesses. Its `!mmra !{!"amdgcn-av",
!"none"}` metadata can suppress a release's payload writeback or an acquire's
payload invalidation while retaining the atomic access's own visibility.
Thus identical ordering/scope names need not imply identical payload cache
operations. Non-atomic `volatile` global accesses receive SYSTEM availability
or visibility in that model; atomic volatile accesses retain their declared
scope. These target-specific meanings do not turn volatile into a general
producer/consumer synchronization protocol. [AMDGPU access model][llvm-av-model]
[Emitter metadata handling][llvm-fence-builder]

Mesa demonstrates another implementation strategy. RADV runs
`nir_lower_memory_model`, which transfers applicable make-available/make-visible
requirements onto loads/stores as `ACCESS_COHERENT`. AMD's cache-policy builder
then selects per-access fields, including `glc`/`dlc` or GFX12 scope. ACO tracks
ordering and execution barriers separately. The absence of an LLVM-style
invalidate next to a barrier consequently does not imply the absence of a
visibility operation: the affected accesses can carry it instead. This
conclusion depends on the preceding lowering and its actual flags, rather
than on a mnemonic comparison alone. [RADV caller][mesa-memory-model-caller]
[NIR availability/visibility lowering][mesa-memory-model]
[AMD access fields][mesa-cache-flags] [ACO ordering][mesa-waits]
[ACO execution barrier][mesa-barrier]

Likewise, a non-temporal access describes cache allocation/reuse policy, not
an ownership transfer. Mesa's GFX10 policy explicitly distinguishes coherent
bypass from non-coherent bypass and states that the latter does not guarantee
ordering with coherent stores. [Access-policy distinctions][mesa-cache-flags]

[Cache residency and replacement policy](cache-residency.md) separates RDNA4
and CDNA5 temporal hints from persisting-size requests and HIP access windows.
Those controls retain the same independent publication and final-use edges.

## Resident transfer and reuse

For a shader-generated SDMA transfer, the complete actor flow is:

1. The native owner creates accessible command, payload, control and
   notification mappings. Their cache policies, atomic widths and final
   users are known independently of the shader target.
2. Shader contributors finish preparing source data and command bytes. The
   publisher joins those contributors and releases each resource to its
   actual reader before making the command extent visible through WPTR.
   An in-stream SDMA cache packet cannot repair missing visibility of the
   bytes needed to fetch that packet.
3. SDMA consumes the commands, transfers payload and applies its required
   release before publishing completion. The native completion protocol
   establishes what that observation covers.
4. The shader observes completion through a fresh control path, acquires
   the destination, and lets its participating readers consume it. A marker
   in uncached memory can coexist with cached payload needing acquisition.
5. Every payload reader finishes before the shader release-publishes credit.
   Command-ring space, copied payload slots and notification mappings retain
   their separate final-user boundaries through shutdown.

The [device-publication chapter](sdma/device-publication.md) supplies the
reservation, native WPTR/doorbell and drain details. The same two directed
payload edges compose CPU or NPU participation only when that participant's
own publication/completion mechanism supplies the matching half. The
[pipeline recipe](../interop/pipelines.md) retains those ownership and progress
conditions across several stages.

[llvm-order]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7485-L7604
[rdna4-vector-order]: https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna4-instruction-set-architecture.pdf
[llvm-alias-waits]: https://github.com/llvm/llvm-project/blob/1dac88b2aad1af472a4f4d5b7d87930e6e557d1f/llvm/lib/Target/AMDGPU/SIInsertWaitcnts.cpp#L2460-L2465
[cdna5-counters]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=61
[cdna5-scopes]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=44
[cdna5-xml]: https://gpuopen.com/download/AMD_GPU_MR_ISA_XML_2026_08_06.zip
[llvm-gfx125-nv]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L17832-L17882
[llvm-release-builder]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L441-L461
[llvm-load-builder]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L2325-L2367
[llvm-legacy-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7669-L7726
[llvm-legacy-scalar]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7728-L7755
[llvm-gfx942-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11208-L11306
[llvm-gfx10-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L13566-L13695
[llvm-gfx12-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L15580-L15726
[llvm-gfx125-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L17713-L17753
[llvm-gfx125-order]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L17755-L17861
[llvm-gfx12-waits]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1958-L2014
[llvm-barrier-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7606-L7634
[llvm-gfx10-load]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1552-L1585
[llvm-gfx10-waits]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1671-L1696
[llvm-gfx12-workgroup]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1922-L1946
[llvm-tgsplit]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1256-L1276
[llvm-legacy-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1371-L1482
[llvm-cdna-wb]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1497-L1550
[llvm-cdna-load-store]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1047-L1154
[llvm-cdna-rmw]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1155-L1191
[llvm-gfx942-release]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L12108-L12220
[llvm-gfx10-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1803-L1823
[llvm-gfx12-release]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L16443-L16500
[llvm-gfx12-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L15971-L15997
[llvm-gfx125-release]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L18743-L18822
[llvm-gfx125-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L18110-L18140
[llvm-cache-dispatch]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1036-L1045
[llvm-cdna-features]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/AMDGPU.td#L2064-L2131
[llvm-rdna-features]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/AMDGPU.td#L2315-L2335
[llvm-gfx12-wb]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L2108-L2147
[llvm-gfx12-inv]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L2034-L2087
[llvm-gfx125-cache-predicate]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/GCNSubtarget.h#L745
[llvm-gfx12-store-feature]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/AMDGPU.td#L2357-L2426
[llvm-special-accesses]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L2183-L2238
[llvm-xcnt-predicate]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/GCNSubtarget.h#L1023-L1028
[llvm-system-store-waits]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L1875-L1889
[llvm-fence-spaces]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7636-L7664
[llvm-av-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUMemoryModel.md#L209-L300
[llvm-fence-builder]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/SIMemoryLegalizer.cpp#L2446-L2482
[mesa-cache-flags]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_shader_util.c#L1082-L1210
[mesa-memory-model-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_pipeline.c#L283-L292
[mesa-memory-model]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/compiler/nir/nir_lower_memory_model.c#L104-L156
[mesa-waits]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/compiler/aco_insert_waitcnt.cpp#L454-L538
[mesa-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/compiler/aco_lower_to_hw_instr.cpp#L2959-L2977
[copy-contract]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2153
