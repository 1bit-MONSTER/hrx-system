# Compiled compute dispatch

A raw PM4 compute dispatch consumes live shader registers. The command processor
does not infer a program's argument ABI or all runtime policy from its entry
address. The caller combines compiler resource requirements, per-dispatch
arguments and geometry, and the native queue's runtime-owned state before
launching the program. PAL's compute path makes this split explicit: a changed
pipeline emits program state, then the dispatch path realizes user data and
arguments. [Pipeline and user-data binding][pal-program-bind]

## Applicability and executable ownership

The compiled-register and GCR examples below follow GFX10/GFX11 compute
sources, with GFX11 instruction-prefetch details identified explicitly. The
dispatch packet and control tables separately identify older layouts, GFX12
graphics/compute paths and physical GC12.1 fields. A common opcode does not
make those register layouts or engine policies interchangeable. The Linux
GC9.4.3 trap example describes a different native context and is not a claim
that an RDNA launch can be copied to CDNA. A native [AQL dispatch](../aql/README.md)
consumes a descriptor through a different firmware interface; PM4 carried by an
AQL vendor packet does not automatically inherit the same descriptor handling.

The program owner retains the linked executable sections, resolved relocations,
entry alignment and readable instruction-fetch envelope. A function symbol's
instruction count is not the complete required backing. On GFX11:

| Extent | Source contract |
| --- | --- |
| Program address | PGM_LO/HI represent the entry in 256-byte units. The raw KFD builder splits that shifted address; PAL validates 256-byte alignment when realizing its program register. |
| RSRC3 prefetch | `INST_PREF_SIZE` is measured in 128-byte units. It describes an initial request, not the lifetime of all instruction fetches. |
| Compiler text tail | LLVM's HSA finalization emits `s_code_end` padding. For GFX11, `EmitCodeEnd` aligns to 128 bytes and adds 384 bytes for prefetch mode 3. |
| Upload allocation | PAL reserves readable padding after uploaded sections. Its GFX11 padding is three 64-byte units, separate from the compiler's text tail. |

[Raw program address][kfd-program-address] [PAL address realization][pal-program-address]
[RSRC3][llvm-prefetch]
[Compiler finalization][llvm-finalization] [Tail emission][llvm-tail]
[Uploader][pal-uploader] [Allocation unit][pal-line] [Padding policy][pal-padding]

Linux's GFX11 process setup selects `SH_MEM_CONFIG.INITIAL_INST_PREFETCH=3`.
That is native context policy, not a compiler property. An extracted executable
must retain both its linked address relationships and sufficient readable
backing; moving only a function body or treating its symbol size as the fetch
boundary discards those requirements. [Native prefetch setup][linux-prefetch]

## Initial-register and argument ABI

Compiler metadata specifies enabled user and system SGPR inputs, VGPR inputs,
wave size, fixed group/private storage and argument layout. Enabled SGPR inputs
are packed in the specified order. For example, a kernel enabling only the
kernarg pointer among user inputs, with no kernarg preload, receives that pointer
in `s[0:1]`; if group X is enabled, it follows in `s2`. Additional enabled inputs
change that layout.
GFX11 packs enabled local workitem coordinates in `v0`; disabled components
and reserved high bits have defined zero behavior. [Initial-register ABI][llvm-inputs]

The caller supplies every input the program actually consumes: explicit
arguments, compiler hidden arguments, dispatch/queue pointers, scratch state or
preloaded kernargs when required. Semantic argument size, ABI alignment, actual
scalar-load footprint and host-language structure size are separate quantities.
A host structure's padding is not an argument value, but every byte fetched by
the compiled entry must have valid backing. The [indirect-dispatch](indirect.md)
chapter describes an additional distinction between CP count fetches and
shader-visible grid-size inputs.

## Register binding and launch

PAL's SET_SH_REG builder uses opcode `0x76` and a DWORD register offset from
`0x2c00`, passing `ShaderCompute` through its shared header builder. The
[register-transport chapter](registers.md) separates the engine-specific header
views, indexed and pair forms, memory-backed loads and their lifetimes. The
following intervals are the ordinary GFX11 compute binding surface, not a
requirement to overwrite all neighboring queue-context registers.
[Register map][pal-registers]
[Packet construction][pal-set] [Compute binding][pal-binding]

| Register interval | Meaning |
| --- | --- |
| `0x2e0c..0x2e0d`, `COMPUTE_PGM_LO`, `COMPUTE_PGM_HI` | Program entry address in 256-byte units. |
| `0x2e12..0x2e13`, `COMPUTE_PGM_RSRC1`, `COMPUTE_PGM_RSRC2` | Compiler resource and mode fields combined with applicable runtime trap/scratch state and realized LDS allocation. |
| `0x2e28`, `COMPUTE_PGM_RSRC3` | Generation-specific prefetch and resource fields. |
| `0x2e15`, `COMPUTE_RESOURCE_LIMITS` | Launch resource/scheduling policy, including `SIMD_DEST_CNTL`. |
| `0x2e04..0x2e06`, `COMPUTE_START_X`, `COMPUTE_START_Y`, `COMPUTE_START_Z` | Start coordinates. |
| `0x2e07..0x2e09`, `COMPUTE_NUM_THREAD_X`, `COMPUTE_NUM_THREAD_Y`, `COMPUTE_NUM_THREAD_Z` | Local workgroup dimensions. |
| `0x2e40` onward, `COMPUTE_USER_DATA_0` and following registers | ABI-selected user inputs, such as the low/high kernarg pointer. |

The compiler's target-specific resource words describe register allocation,
enabled inputs and program modes. Their descriptor field definitions remain
distinct from the live overrides: the launch owner realizes
[`COMPUTE_PGM_RSRC2.LDS_SIZE`](lds.md#compiler-requirement-and-launch-allocation)
and combines applicable trap/scratch policy as described below. Copying the
compiler words without those native inputs is not a complete raw launch.
[Compiler resource-word definitions][llvm-resource-words]

The geometry interval ends before PIPELINESTAT_ENABLE and PERFCOUNT_ENABLE.
Those enables belong to native queue/profiling policy. Linux's MQD initializer
also establishes unordered dispatch and other queue context. A shader binding
is not authority to replace these fields. [GFX11 MQD initialization][linux-mqd]

The bound program's wave size must be legal for its
[target family](../architectures.md#wavefront-modes). For example, GFX11 supports
wave32 and wave64; GFX12.5 supports only wave32. The source-specific initiator
layouts below do not extend either mode availability or packet layout to
another target. `CS_W32_EN` realizes the compiled mode; changing that bit does not
convert an executable between widths.

PAL derives SIMD_DEST_CNTL from whether
`ceil(workgroup_x * workgroup_y * workgroup_z / wavefront_size)` is a multiple
of four, with settings that can override the choice. Thus a 128-workitem group
contains four wave32 waves or two wave64 waves; changing programs can change
this policy even when the workgroup dimensions stay the same. The wave size
used here and in the dispatch initiator belongs to the newly bound program.
This scheduling policy is separate from LDS allocation and does not make
dispatch packets synchronous. [Resource-limit derivation][pal-limits]

## Dispatch packets and launch controls

The ordinary `DISPATCH_DIRECT` packet, opcode `0x15`, has five DWORDs in
PAL's MEC, ME and PFP definitions. All payload words are 32 bits:

| Word | Representation |
| --- | --- |
| 0 | Type-3 header: type 3 in bits 31:30, count 3 in bits 29:16 and opcode in bits 15:8. Low header controls have engine-specific views. |
| 1, 2, 3 | `dim_x`, `dim_y`, `dim_z`; units and start/end interpretation follow the selected launch mode. |
| 4 | `dispatch_initiator`, containing `COMPUTE_DISPATCH_INITIATOR`. |

[Merged MEC view][pal-direct-fields] [Merged ME view][pal-me-direct-fields]
[GFX12 MEC view][pal12-direct-fields] [GFX12 PFP view][pal12-pfp-direct-fields]

Direct dimensions are copied into command storage during recording. That
captures those scalar values, not the executable, arguments or payload to
which the live registers refer. Their backing remains independently owned
through the last dispatch using it; the command allocation follows its native
submission owner's retirement protocol. [Packet construction][pal-dispatch]
[Command lifetime](command-buffers.md)

PAL's direct builders pass `ShaderCompute` to their shared header builder,
giving ordinary unpredicated header `0xc0031502`, even though the generated
MEC header names its low byte reserved. Compute callers disable the packet
predicate and use a separate `COND_EXEC` when conditional execution is needed.
Graphics callers can supply the packet predicate. These are actual emitter
conventions, not evidence that all low-bit combinations are interchangeable.
[Direct builder][pal-dispatch] [GFX12 builder][pal12-direct]
[Compute caller][pal12-compute-dispatch] [Header views](registers.md#inline-consecutive-writes)
[Conditional execution](conditional.md)

### Initiator field family

The following fields share positions in the cited Mesa GFX6–GFX12 register
databases, PAL merged/GFX12 definitions and Linux native register masks, with
the GC12.1 naming and width differences called out explicitly. Their presence
in a definition establishes representation; the caller policies below identify
which combinations are actually emitted.
[Mesa legacy layout][mesa-initiator6] [Mesa GFX9/CDNA layout][mesa-initiator940]
[PAL merged layout][pal-initiator] [PAL GFX12 layout][pal12-initiator]
[GC12.1 layout][linux-initiator121]

| Bit(s) | Field | Meaning and source boundary |
| --- | --- | --- |
| 0 | `COMPUTE_SHADER_EN` | Enables the dispatch. Ordinary builders set 1. |
| 1 | `PARTIAL_TG_EN` | Enables the programmed partial-group dimensions; used by Mesa's partial final-group paths. |
| 2 | `FORCE_START_AT_000` | Overrides `COMPUTE_START_X/Y/Z` with zero. Offset launches leave this clear. |
| 3 | `ORDERED_APPEND_ENBL` | The Sea Islands guide associates this with generated wave identities for ordered append. The inspected ordinary builders leave it clear. |
| 4 | `ORDERED_APPEND_MODE` | The legacy guide distinguishes per-wave identity (0) from per-workgroup identity (1). A zero in an ordinary builder does not enable ordered append. |
| 5 | `USE_THREAD_DIMENSIONS` | Supplies workitem dimensions for hardware group/partial-dimension calculation instead of ordinary workgroup dimensions. |
| 6; GC12.1 7:6 | `ORDER_MODE` | The older one-bit form selects ordered (0) or unordered (1) wave launch. GC12.1's public mask is two bits; the one-bit interpretation does not define its additional encodings. |
| 10 | `SCALAR_L1_INV_VOL`; GC12.1 `SCALAR_L0_INV` | The legacy guide specifies volatile scalar-cache invalidation for the dispatch. The newer name alone does not establish an equivalent cache protocol. |
| 11 | `VECTOR_L1_INV_VOL`; GC12.1 `VECTOR_L0_INV` | The legacy guide specifies volatile vector-cache invalidation on participating CUs. These controls do not describe producer completion or a full payload release/acquire. |
| 14 | `RESTORE` | The legacy guide assigns this to internal context-switch restore logic; ordinary dispatch builders leave it clear. |

[Sea Islands register guide, revision 1.0, pp. 196–197][cik-dispatch]
[RadeonSI geometry and control construction][mesa-si-controls]

The remaining field positions vary by source family:

| Source family | Additional declared fields; unlisted positions are unnamed in that source |
| --- | --- |
| Mesa GFX6, GFX7, GFX8 and GFX8.1; corresponding Linux GCA masks | Bits 9:7 `DISPATCH_CACHE_CNTL`, bit 12 `DATA_ATC`. Bit 13 and bits 31:15 are unnamed. The legacy guide assigns bits 9, 8 and 7 to scalar-L1, L2 and vector-L1 disable controls, respectively. |
| Mesa GFX9 and GFX940; Linux GC9 masks | Bits 9:7 are unnamed and bit 12 is explicitly `RESERVED`; no wave32 or tunnel field. |
| Mesa GFX10/GFX10.3 and Linux GC10.1/10.3 | Add bit 13 `TUNNEL_ENABLE` and bit 15 `CS_W32_EN`; bit 12 remains reserved. |
| Mesa GFX11/GFX11.5, PAL's GFX11 overlay and Linux GC11 masks | Add bit 16 `AMP_SHADER_EN` and bit 17 `DISABLE_DISP_PREMPT_EN`; bits 31:18 remain unnamed. |
| Mesa GFX12 and Linux GC12.0 | Bit 12 becomes `PING_PONG_EN`; add bit 18 `INTERLEAVE_2D_EN` and bits 31:29 `TTRACE_QUEUE_ID`. |
| PAL GFX12 | Also names bit 19 `WGS_DISPATCH`, absent from the cited Mesa GFX12 and GC12.0 views. |
| Linux GC12.1.0 | Adds bit 20 `CLUSTER_EN` as well as bit 19 `WGS_DISPATCH`; changes `ORDER_MODE` and the cache-field names as above. Bits 9:8 and 28:21 remain unnamed. |

[GFX6–8 register masks][linux-initiator8] [GFX9 masks][linux-initiator9]
[GFX10 masks][linux-initiator10] [GFX11 masks][linux-initiator11]
[GC12.0 masks][linux-initiator120] [Mesa GFX12 layout][mesa-initiator12]
[PAL GFX12 layout][pal12-initiator] [GC12.1 layout][linux-initiator121]

`AMP_SHADER_EN` belongs to the task/amplification dispatch path, which also
has task-ring operands and a graphics consumer. Its presence in the shared
initiator does not make a normal compute launch a task/mesh dispatch.
`WGS_DISPATCH`, `TTRACE_QUEUE_ID`, `DATA_ATC` and the GC12.1 extended modes have
no ordinary software-selected semantics established by the cited direct and
indirect callers. A named bit is not a complete programming sequence.
[Task dispatch construction][pal-task-dispatch] [Cluster execution](../clusters.md)

### Dimensions, starts and partial workgroups

`COMPUTE_START_X/Y/Z` and `COMPUTE_DIM_X/Y/Z` are 32-bit coordinates. PAL's
ordinary offset caller writes `START=offset`, clears `FORCE_START_AT_000`
and emits `DIM=offset+launchSize`. The packet carries end coordinates, while
the separate `logicalSize` goes to shader-ABI setup. For example, X start 3
and five launched groups produce packet X value 8. RADV independently adds
nonzero base offsets to its launch counts. Shader-visible logical dimensions
and the packet's end coordinates are therefore separate inputs.
[PAL offset caller][pal-offset] [GFX12 offset caller][pal12-offset]
[RADV base and geometry handling][mesa-geometry]

The local group registers use the following representation. The source fields
are encoding capacities, not maximum legal workgroup sizes:

| Register view | Low 16 bits | High 16 bits |
| --- | --- | --- |
| GFX6–GFX11 `COMPUTE_NUM_THREAD_X/Y/Z` | Bits 15:0 `NUM_THREAD_FULL`. | Bits 31:16 `NUM_THREAD_PARTIAL`. |
| GFX12 `COMPUTE_NUM_THREAD_X/Y` | Bits 12:0 `NUM_THREAD_FULL`; bits 15:13 `INTERLEAVE_BITS_X` or `INTERLEAVE_BITS_Y`. | Bits 31:16 `NUM_THREAD_PARTIAL`. |
| GFX12 `COMPUTE_NUM_THREAD_Z` | Bits 15:0 `NUM_THREAD_FULL`. | Bits 31:16 `NUM_THREAD_PARTIAL`. |

[Mesa legacy dimensions][mesa-initiator6] [GFX12 dimensions][mesa-dimensions12]

For a direct unaligned launch, RADV rounds each workitem extent up to a group
count, programs the ordinary full size and the final group's remainder, and
sets `PARTIAL_TG_EN`. An exactly divisible dimension uses the full size as
its remainder, not zero. Its offsets must be divisible by local group sizes
before conversion to group coordinates. RadeonSI similarly replaces zero
`last_block` components with full sizes when another component needs a
partial group. The executable must permit partial groups: encoding a remainder
does not remove a compiled uniform-workgroup requirement or change its
barrier contract. [RADV conversion][mesa-geometry] [RadeonSI final groups][mesa-si-controls]
[Executable geometry](../aql/dispatch.md#arguments-geometry-and-initial-registers)

The workitem-dimension path is distinct: the WDDM builder supplies its workitem
grid with `USE_THREAD_DIMENSIONS=1`, whereas PAL's ordinary compute command
uses group counts. RADV's indirect unaligned path also sets the workitem bit.
RADV has another direct conversion for `CHIP_ICELAND` or `CHIP_TONGA` on its
compute queue. That workaround predicate differs from the GFX7-only
32-byte indirect-address repair. [WDDM launch][dxg-launch]
[PAL compute launch][pal12-compute-dispatch] [RADV indirect path][mesa-dispatch]
[Separate device predicates][mesa-geometry-bugs]

## Launch policy and engine selection

Launch order, preemption, workgroup distribution and shader completion have
different owners:

| Caller | Selected launch controls |
| --- | --- |
| PAL ordinary direct and MEC indirect | Initialize the initiator to zero, then set enable, start/thread mode, compiled wave mode, tunneling and partial-preemption policy. Set `ORDER_MODE=1`. Graphics-indirect builders instead leave `ORDER_MODE=0`. |
| RADV ordinary compute | Default unordered launch only for `gfx_level >= GFX7 && (family < CHIP_GFX940 || has_graphics)`, then clear it for an explicitly ordered dispatch. Set tunneling for GFX10+; its comment attributes effective high-priority-queue permission to KMD. Its task initiator separately disables partial preemption. |
| RadeonSI ordinary compute | Uses the same generation/graphics predicate for unordered launch, additionally requiring no ordered-atomic-add shader. Compiled wave size selects `CS_W32_EN`. |
| WDDM `BuildDispatch` | Sets enable, force-zero-start and workitem dimensions, plus compiled wave32 mode. It does not inherit PAL's unordered or tunneling defaults. |

[PAL direct][pal-dispatch] [PAL graphics indirect][pal-indirect-policies]
[PAL MEC indirect][pal-mec-dispatch]
[RADV defaults][mesa-launch-defaults] [RADV dispatch override][mesa-dispatch]
[RadeonSI controls][mesa-si-controls] [WDDM launch][dxg-launch]

PAL's partial-preemption input originates in
`ComputePipelineCreateInfo::disablePartialDispatchPreemption`. Its API
describes workgroup-level preemption when CWSR is unavailable and why
interdependent workgroups may require this selection. The pipeline retains
the client flag and compute callers forward it. The older builder ORs bit 17
through a constant, without a GFX11 guard, even though its generated register
names that bit in a GFX11 overlay and parameter comments mention GFX10.
The exact emitter and API policy establish this distinction; the overlay is
not an exhaustive firmware-admission predicate. GFX12's builder names the bit
directly. Universal-queue direct and offset callers pass false in both PAL
families; graphics-indirect has no partial-preemption input. The pipeline flag
therefore does not apply to every caller of the shared direct builder.
[Universal GFX10/GFX11 callers][pal-universal-preempt]
[Universal GFX12 caller][pal12-universal-preempt]
[Earlier offset caller][pal-universal-offset-preempt]
[GFX12 offset caller][pal12-offset-interleave]
[Client contract][pal-preempt-api] [Stored policy][pal-preempt-owner]
[Bit constant][pal-preempt-bit] [GFX12 builder][pal12-direct]

This flag does not disable operating-system timeouts or grant a dispatch an
unbounded lifetime. CWSR and queue scheduling remain native context contracts.
Likewise, `ORDER_MODE` orders wave launch rather than publishing one shader's
stores to another; the execution/cache dependency below is still necessary.
[Context save](../context-save.md) [Scheduling](../scheduling.md)

### GFX12 distribution controls

GFX12 has two different interleaves. `COMPUTE_NUM_THREAD_X/Y.INTERLEAVE_BITS_*`
describes thread tiling within a workgroup. RadeonSI selects `(1,1)` for
`DERIVATIVE_GROUP_QUADS`, giving a 2×2 tile. For `DERIVATIVE_GROUP_NONE` it
chooses among 8×8, 4×8, 4×4 and 2×2 according to divisible local dimensions;
`DERIVATIVE_GROUP_LINEAR` leaves those fields clear. This is distinct
from `COMPUTE_DISPATCH_INTERLEAVE`, which distributes groups across shader
engines. [RadeonSI tiling][mesa-si-interleave]

| `COMPUTE_DISPATCH_INTERLEAVE` bits | GFX11 name | GFX12 name and PAL representation |
| --- | --- | --- |
| 9:0 | `INTERLEAVE` | `INTERLEAVE_1D`, thread count for the selected one-dimensional policy. |
| 19:16 | Unnamed | `INTERLEAVE_2D_X_SIZE`, log2 of groups along X in the selected subgrid. |
| 27:24 | Unnamed | `INTERLEAVE_2D_Y_SIZE`, log2 of groups along Y in the selected subgrid. |
| 15:10, 23:20, 31:28 | Unnamed | Unnamed. |

[Register layouts][pal12-initiator] [GFX11 register][pal11-interleave]
[PAL interleave construction][pal12-interleave-build]

For GFX11, PAL selects default 64 or explicit 64/128/256/512, with 1 for
`Disable`; a device setting can override the client choice. Its conversion
is explicitly guarded by `IsGfx11`, independently of the source directory's
`gfx9` name. [GFX11 selection][pal11-interleave-policy]

PAL's explicit one-dimensional selections emit 64, 128, 256 or 512 threads;
its `Disable` choice emits 1. RadeonSI's comment instead lists 0 as disabled,
while its ordinary default emits 256. These source choices do not establish
that 0 and 1 are interchangeable. PAL's default two-dimensional choice derives
a power-of-two group subgrid from local dimensions, limits its area to 16
groups and has a distinct `PAL_BUILD_NAVI48 && isNavi48` branch for 2D groups.
An explicit client selection and the default-derived pattern have different
fallback policies. [PAL construction][pal12-interleave-build]
[RadeonSI policy][mesa-si-interleave]

PAL's GFX12 compute callers pass both ping-pong and 2D interleave as false,
with comments identifying ACE incompatibility. On the graphics queue,
`ValidateDispatchPalAbi` can replace a default 2D pattern with 1D for indirect
dispatches when its setting disallows them, or for small direct dispatches.
The small-dispatch predicate compares logical X/Y sizes against configured
minima and their product against the selected subgrid area. Explicit client
patterns do not use those default-only tests. [Compute path][pal12-compute-dispatch]
[Graphics selection][pal12-interleave-select]

For offset launches, PAL also requires X/Y offsets aligned to the selected
group subgrid. With 2D enabled, it writes `START_X/Y=offset >> log2(subgrid)`;
Z is unchanged, while packet dimensions still use `offset+launchSize`.
Misaligned offsets select the ordinary 1D form. This mode-specific conversion
is why a 2D launch cannot simply reuse ordinary start coordinates.
[Offset conversion][pal12-offset-interleave]

`PING_PONG_EN` reverses group walk order in PAL's builder contract. PAL's
graphics caller honors the pipeline's reverse-order request, can alternate
recorded dispatches from a command-buffer flag, and has separate force-on/off
settings. Offset dispatches leave it clear. RadeonSI alternates it only on
its graphics queue, without partial groups or an ordered-atomic-add shader.
It does not enable its 2D-dispatch selection: that block is compiled out in
the cited revision. Thread tiling and ping-pong can therefore be active there
without selecting either interleaved dispatch opcode.
[PAL walk policy][pal12-ping-pong] [RadeonSI active and disabled paths][mesa-si-interleave]

PAL's builders require `PARTIAL_TG_EN=0` and `USE_THREAD_DIMENSIONS=0` for
ping-pong; 2D dispatch additionally requires `ORDERED_APPEND_ENBL=0`.
These distribution controls provide neither execution dependencies nor a
measured speedup by themselves. [Builder constraints][pal12-direct]

### Interleaved direct packet views

`DISPATCH_DIRECT_INTERLEAVED`, opcode `0xa7`, has distinct GFX12 source
representations. PAL's actual builder selects the PFP form:

| View | Header and payload words |
| --- | --- |
| PFP, five DWORDs | Word 0 type-3/count 3; words 1–3 32-bit X/Y/Z dimensions; word 4 initiator. |
| ME, seven DWORDs | Word 0 type-3/count 5; word 1 `dim_z`; words 2–3 `prescale_dim_x/y`; words 4–5 `dim_x/y`; word 6 initiator. Every payload word is 32 bits. |

[PFP definition][pal12-pfp-interleaved] [ME definition][pal12-me-interleaved]
[Actual builder][pal12-direct] [Opcodes][pal12-dispatch-opcodes]

The ME structure is not the layout written by that caller. The public
definitions and PFP producer do not establish an application-visible sequence
for authoring the ME prescale operands. The [indirect chapter](indirect.md#interleaved-indirect-packet-views)
records the analogous `0xa8` distinction, including its different field widths.

## Shader wait-counter mode: MEM_ORDERED

In the GFX10–GFX11 `vmcnt`/`vscnt` model,
`COMPUTE_PGM_RSRC1.MEM_ORDERED`, bit 30, controls how a wave's vector-memory
wait counters account for completed instructions. The compiled waits and the
bound register must agree about this mode. LLVM's descriptor table describes
the following behavior; it reserves the bit as zero for GFX6–GFX9.
[Descriptor field][llvm-mem-ordered]

| Value | Completion accounting in the descriptor's `vmcnt`/`vscnt` model |
| --- | --- |
| 0 | Loads and atomics returning a value report completion out of order relative to sample instructions through `vmcnt`. Stores and atomics without a return value report completion in order through `vscnt`. |
| 1 | Loads, atomics returning a value and sample instructions report completion in order through `vmcnt`. Stores and atomics without a return value still report completion in order through `vscnt`. |

This is instruction-completion accounting within a wave. It supplies neither
a cross-agent execution dependency nor the [cache operations](cache.md) needed
to publish or acquire payload data. LLVM's GFX10–GFX11 AMDHSA memory model selects
in-order load/sample reporting. Other programming models can select the mode
from the actual program: Mesa's ACO starts with `mem_ordered=false` and sets
it for `GFX10 <= gfx_level < GFX12` when a wait distinguishes outstanding
vector-memory instruction classes whose completions need ordering. RADV carries
that result into compute RSRC1 for `GFX10 <= gfx_level <= GFX11_7`.
[AMDHSA mode][llvm-amdhsa-waits] [ACO initialization][mesa-wait-initialization]
[Wait analysis][mesa-wait-analysis] [Compute binding][mesa-wait-binding]

Linked code retains the combined requirement. RADV ORs `mem_ordered` across
ray-tracing shader configurations before realizing the resource word. PAL's
compute pipeline consumes the ABI metadata and ORs the bit from linked shader
libraries. Reusing only the entry shader's original word can lose a callee's
requirement. The neighboring `FWD_PROGRESS` field is a separate mode.
[RADV configuration merge][mesa-wait-linking] [PAL metadata][pal-wait-metadata]
[PAL library linking][pal-wait-linking]

GFX12 applicability differs across the cited sources. LLVM's descriptor table
labels this field GFX10–GFX12, and PAL's GFX12 register definition retains bit
30. LLVM's GFX12 memory-model chapter instead describes separate load, store,
sample and BVH counters, with completion ordered within each type. Mesa excludes
GFX12 from its `MEM_ORDERED` setting and tracks sample and BVH waits separately
there.
[GFX12 register][pal-gfx12-mem-ordered] [GFX12 wait model][llvm-gfx12-waits]
[Separate wait events][mesa-gfx12-waits]

AMD's RDNA4 ISA guide, dated 7 April 2025, independently describes those
separate counters in §5.7 and Table 26. It distinguishes counter retirement
from memory order: loads can write VGPRs out of order while their counter
reports completion in order; stores to different addresses need not preserve
issue order. For stores, `STOREcnt` decrements after the write reaches the
memory-hierarchy level selected by `SCOPE`.
Global invalidation uses `LOADcnt`, while global writeback and
writeback/invalidation use `STOREcnt`; issuing the cache instruction alone
does not establish its completion. [RDNA4 dependency rules, pp. 51–55][rdna4-waits]

Section 5.7 does not define `MEM_ORDERED` bit 30.
The named field, separate-counter model and Mesa's selection policy therefore
remain distinct evidence; they do not establish the effect of changing the
bit on GFX12.

## Runtime trap and context state

LLVM's GFX6–11 descriptor contract requires zero in compiler-owned storage for
TRAP_PRESENT and certain exception/trap controls; runtime descriptor processing
supplies the live policy. A raw PM4 caller cannot conclude that the live register
must be zero. The libhsakmt raw dispatch builder explicitly sets TRAP_PRESENT
for pre-GFX12 targets. [Descriptor RSRC2][llvm-trap] [Descriptor RSRC3][llvm-prefetch]
[Raw dispatch builder][hsakmt-trap]

KFD's GC11 software context-save workaround also requires
`COMPUTE_PGM_RSRC1.PRIV` (bit 20). The native queue manager disables hardware
traps for GC11.0.0 through GC11.0.3, and ROCm's raw PM4 builder starts affected
waves with PRIV set. The context-restore program retains PRIV when returning
to a wave whose hardware traps are disabled. These are native execution-context
requirements; the compiler descriptor does not select them.
[Native predicate][linux-cwsr-workaround] [MES queue setup][linux-mes-traps]
[Raw dispatch builder][hsakmt-trap] [Context restore][linux-cwsr-return]

The native GC revision and the compiler target suffix are different identities:

| Native GC | KFD compiler target | Software-trap PRIV requirement |
| --- | --- | --- |
| 11.0.0 | `gfx1100` | Set |
| 11.0.1 | `gfx1103` | Set |
| 11.0.2 | `gfx1102` | Set |
| 11.0.3 | `gfx1101` | Set |
| 11.0.4 | `gfx1103` | Absent |

KFD's identity mapping and workaround predicate establish this table. ROCm's
equivalent compiler-target predicate uses array count to distinguish the two
`gfx1103` implementations. Its correction explicitly reports that applying
PRIV to unaffected GFX11 parts can hang the system. Consequently, a general
GFX11 check or a compiler-target suffix alone cannot select the workaround.
[Native identity mapping][linux-gc11-targets] [ROCm predicate][hsakmt-cwsr-predicate]
[ROCm correction][hsakmt-cwsr-correction]

Linux GC9.4.3 gives a concrete conditional owner flow:

1. Enabled CWSR selects KFD's built-in context-save handler.
2. The 64-bit V9 aperture reserves its addresses. Successful process-VM
   initialization allocates and installs the first-level TBA/TMA.
3. Queue creation copies the process handler state into the queue. V9 MQD
   initialization sets TRAP_PRESENT when TBA is nonzero, independently of
   AQL versus PM4 format.
4. When the CWSR mapping exists, a subsequently registered runtime trap handler
   is chained at the second level rather than replacing KFD's first level.

[CWSR selection][linux-cwsr-policy] [Address reservation][linux-cwsr-aperture]
[VM initialization][linux-cwsr-vm] [Handler installation][linux-cwsr-install]
[Queue inheritance][linux-queue-trap] [MQD trap field][linux-v9-trap]
[Second-level registration][linux-trap-chain]

The read-only `cwsr_enable` parameter and successful construction on that exact
path can establish a deployment-specific first-level handler. Nonzero
`cwsr_size` or `ctl_stack_size` cannot: their calculation derives storage from
topology without reporting whether a handler is installed. Debugger and
exception policy require their own process owner information.
[CWSR parameter][linux-cwsr-enable] [Storage calculation][linux-cwsr-size]

The [context-save storage contract](../context-save.md) explains the native
capacity calculation, per-XCC layout and MQD frontiers. It also separates
saved-wave inspection, dispatch completion and final queue removal.

ROCr's native vendor-1 AQL wrapper copies PM4 bytes and supplies completion;
its CPU implementation does not merge shader resource fields. The separate
DXG/WDDM software translator copies descriptor resources and adds LDS, while
native AQL bypasses that translation. Neither establishes an implicit
native-firmware merge for a shader-bearing PM4 carrier.
[Native wrapper][rocr-pm4-carrier] [Software translation][dxg-dispatch]
[Native-mode branch][dxg-queue-mode]

## Publication and shader dependencies

The native sequence keeps command publication separate from shader memory
ordering:

```text
initialize executable, arguments and inputs
  → publish command storage through the queue's host-producer protocol
  → acquire the instruction/argument/data caches required by their mappings
  → bind program and arguments → dispatch
  → join the producer shader stage → release producer stores
  → acquire the next consumer's caches → consume
```

For GFX10+, plain ACQUIRE_MEM performs cache work without waiting for shader
idle. Mesa emits a compute completion event before its cache operations; PAL
similarly joins preceding compute work before trailing ACQUIRE_MEM. Command
order alone therefore cannot implement a shader-to-shader payload dependency.
The applicable CS_PARTIAL_FLUSH firmware predicate and its EOP/wait alternative
are described in [memory commands](memory-commands.md#compute-completion-and-firmware).
[Cache-only acquire][mesa-acquire] [Mesa ordering][mesa-flush]
[PAL barrier][pal-barrier] [All-prior-dispatch join][pal-idle]

Changing immutable programs requires a complete compatible binding for each
program and its per-dispatch inputs. PAL re-emits state on a changed pipeline;
Mesa independently tracks program and variable group size. A binding transition
can exist without a data dependency, while a consumer of the preceding output
requires the execution/cache edge above. Restoring a smaller LDS request or
prefetch field is a register-state operation, not evidence of immediate physical
resource reclamation. [PAL program transition][pal-program-bind]
[Mesa binding][mesa-program-bind] [LDS realization](lds.md)

## End-of-pipe release and ownership

PAL's compute queue postamble uses RELEASE_MEM after the pipeline empties to
write a known completion value while writeback/invalidation covers shader
L1/L2 clients. Its generic compute builder chooses BOTTOM_OF_PIPE_TS, end-of-
pipe index 5, TC/L2 destination and write confirmation without an interrupt.
[Queue postamble][pal-postamble] [Release builder][pal-eop]

[Completion publication](release.md) describes the full field and selector
families, native generation differences, event/notification owners and reuse.

The GFX10/GFX11 MEC form has eight DWORDs:

| Word | Fields |
| --- | --- |
| 0 | Type-3 opcode `0x49`, count 6. |
| 1 | Event type bits 5:0; index bits 11:8. GCR starts at bit 12, with 12 bits on GFX10 and 13 on GFX11. Cache policy is bits 26:25. GFX11 additionally defines wait_sync at bit 7 and GLK_INV at bit 30. |
| 2 | Destination bits 17:16, MES notification fields 23:20, interrupt/confirmation bits 26:24 and data selector bits 31:29. |
| 3–4 | Destination address; alignment follows the selected 32/64-bit data width. |
| 5–6 | Immediate data or data-selector-specific operands. |
| 7 | Interrupt context; zero for the ordinary non-interrupting completion. |

[MEC release representation][pal-release-fields] [Mesa release emitter][mesa-release]
[Event enumeration][event-values]

The postamble requests GL2 writeback/invalidation plus GLM, GL1 and GLV
invalidation. BOTTOM_OF_PIPE_TS is event `0x28`; the ordinary known-value
completion uses destination 1, confirmation selector 3 and immediate32 data
selector 1. LRU, MES notification, unused data and interrupt context remain
zero. The [cache-control chapter](cache.md#release_mem-cache-actions) compares
its GCR fields with ACQUIRE_MEM and later native generations. A terminal release
does not acquire instruction/scalar inputs for a later shader, and EOP alone
is not a general CP-DMA drain. The combined DMA-wait feature has its own
[engine/firmware predicate](dma.md#combined-release-wait-and-firmware-identity).

Completion storage remains valid through the event write and its observers.
Shader data, arguments and code remain valid through their last dispatch use;
command bytes additionally obey their submission owner's retirement protocol.
A host-visible completion requires a mapping/cache contract for both payload
and control, followed by the host's acquire observation. A later EOP write to
another address does not by itself settle every prior timestamp store's
visibility and final-use obligation; [timing](timing.md) describes PAL's full
join/writeback sequence.

[pal-program-bind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L675-L807
[pal-binding]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L767-L832
[llvm-prefetch]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6743-L6792
[llvm-finalization]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/AMDGPUAsmPrinter.cpp#L726-L740
[llvm-tail]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/MCTargetDesc/AMDGPUTargetStreamer.cpp#L1089-L1112
[pal-uploader]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/codeObjectUploader.cpp#L291-L305
[pal-line]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L515-L516
[pal-padding]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L5766-L5811
[linux-prefetch]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager_v11.c#L59-L73
[llvm-inputs]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6911-L7071
[llvm-resource-words]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6197-L6792
[pal-registers]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_offset.h#L193-L238
[pal-initiator]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L4613-L4641
[pal-set]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4010-L4027
[linux-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L125-L186
[pal-dispatch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1204-L1246
[mesa-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15047-L15252
[pal-limits]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L300-L345
[llvm-trap]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6564-L6670
[hsakmt-trap]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/Dispatch.cpp#L131-L158
[hsakmt-cwsr-predicate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDBaseComponentTest.cpp#L308-L325
[hsakmt-cwsr-correction]: https://github.com/ROCm/rocm-systems/commit/0a8096b34ae025ce6cff57690157c6e16f9be71f
[linux-cwsr-workaround]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_debug.h#L106-L110
[linux-mes-traps]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L242-L254
[linux-cwsr-return]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/cwsr_trap_handler_gfx10.asm#L1253-L1267
[linux-gc11-targets]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L417-L433
[linux-cwsr-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L512-L575
[linux-cwsr-aperture]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_flat_memory.c#L343-L360
[linux-cwsr-vm]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L1832-L1889
[linux-cwsr-install]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L1511-L1557
[linux-queue-trap]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L2259-L2286
[linux-v9-trap]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L210-L231
[linux-trap-chain]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L1574-L1590
[linux-cwsr-enable]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c#L757-L765
[linux-cwsr-size]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L490-L521
[rocr-pm4-carrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1758
[dxg-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/cmd_util.cpp#L188-L224
[dxg-queue-mode]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L276-L295
[mesa-acquire]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L398-L427
[mesa-flush]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L221-L240
[pal-barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Barrier.cpp#L936-L1011
[pal-idle]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4283-L4302
[mesa-program-bind]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L292-L437
[pal-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9QueueContexts.cpp#L489-L518
[pal-eop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3335-L3537
[pal-release-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1888-L2074
[mesa-release]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L454-L504
[kfd-program-address]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/Dispatch.cpp#L113-L179
[pal-program-address]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L155-L169
[event-values]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_enum.h#L14473-L14511
[llvm-mem-ordered]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6485-L6507
[llvm-amdhsa-waits]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L13546-L13550
[mesa-wait-initialization]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/compiler/aco_insert_waitcnt.cpp#L1118-L1127
[mesa-wait-analysis]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/compiler/aco_insert_waitcnt.cpp#L636-L675
[mesa-wait-binding]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_shader.c#L2493-L2510
[mesa-wait-linking]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_pipeline_rt.c#L1028-L1056
[pal-wait-metadata]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AbiToPipelineRegisters.h#L1615-L1643
[pal-wait-linking]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputePipeline.cpp#L329-L359
[pal-gfx12-mem-ordered]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_registers.h#L2486-L2508
[mesa-gfx12-waits]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/compiler/aco_insert_waitcnt.cpp#L40-L71
[llvm-gfx12-waits]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L15482-L15492
[rdna4-waits]: https://gpuopen.com/download/rdna4-instruction-set-architecture.pdf#page=61
[pal-direct-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L921-L954
[pal12-direct-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L847-L882
[pal12-direct]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L964-L1024
[pal12-compute-dispatch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1436-L1595
[pal12-initiator]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_registers.h#L2299-L2346
[mesa-si-controls]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L628-L674
[pal-task-dispatch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L2173-L2226
[pal-offset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L310-L366
[pal12-offset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1533-L1595
[mesa-geometry]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15151-L15251
[mesa-geometry-bugs]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1021-L1037
[pal-indirect-policies]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1275-L1301
[mesa-launch-defaults]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1640-L1660
[pal-preempt-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palPipeline.h#L320-L329
[pal-preempt-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/computePipeline.cpp#L61-L75
[pal-preempt-bit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Chip.h#L614-L617
[mesa-si-interleave]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L666-L743
[pal12-interleave-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineChunkCs.cpp#L104-L273
[pal12-interleave-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L4157-L4215
[pal12-offset-interleave]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L4584-L4671
[pal12-ping-pong]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L4434-L4468
[pal12-pfp-interleaved]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L1049-L1082
[pal12-me-interleaved]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L837-L882
[mesa-initiator6]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx6.json#L7822-L7843
[mesa-initiator940]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx940.json#L2476-L2490
[mesa-initiator12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx12.json#L10517-L10537
[mesa-dimensions12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx12.json#L10557-L10576
[cik-dispatch]: https://www.x.org/docs/AMD/old/CIK_3D_registers_v2.pdf#page=196
[linux-initiator121]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h#L9553-L9590
[linux-initiator8]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gca/gfx_8_1_sh_mask.h#L8789-L8812
[linux-initiator9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_9_4_3_sh_mask.h#L13195-L13216
[linux-initiator10]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_sh_mask.h#L15370-L15395
[linux-initiator11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_5_0_sh_mask.h#L11385-L11414
[linux-initiator120]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h#L7351-L7384
[pal12-dispatch-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_pm4_it_opcodes.h#L176-L177
[pal-me-direct-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L915-L948
[pal12-pfp-direct-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L1013-L1046
[pal-mec-dispatch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1538-L1572
[dxg-launch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/cmd_util.cpp#L283-L298
[pal-universal-preempt]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L3494-L3503
[pal12-universal-preempt]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L4511-L4518
[pal11-interleave]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L4643-L4652
[pal-universal-offset-preempt]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L3593-L3602
[pal11-interleave-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AbiToPipelineRegisters.h#L1782-L1852
