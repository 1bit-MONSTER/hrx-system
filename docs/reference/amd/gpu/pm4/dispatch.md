# Compiled compute dispatch

A raw PM4 compute dispatch consumes live shader registers. The command processor
does not infer a program's argument ABI or all runtime policy from its entry
address. The caller combines compiler resource requirements, per-dispatch
arguments and geometry, and the native queue's runtime-owned state before
launching the program. PAL's compute path makes this split explicit: a changed
pipeline emits program state, then the dispatch path realizes user data and
arguments. [Pipeline and user-data binding][pal-program-bind]

## Applicability and executable ownership

The register and GCR discussion below follows GFX10/GFX11 compute sources,
with GFX11 instruction-prefetch details identified explicitly. The Linux
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

SET_SH_REG uses opcode `0x76`. Its register operand is a DWORD offset from
`0x2c00`; the compute shader type is selected in the header. The following
intervals are the ordinary GFX11 compute binding surface, not a requirement to
overwrite all neighboring queue-context registers. [Register map][pal-registers]
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

DISPATCH_DIRECT is opcode `0x15`, five DWORDs: header, X/Y/Z dimensions and
DISPATCH_INITIATOR. `USE_THREAD_DIMENSIONS` distinguishes workitem dimensions
from workgroup counts. `FORCE_START_AT_000` selects a zero start;
`CS_W32_EN` must agree with the program's wave mode. PAL disables predication
on the compute engine and supplies independent tunneling/preemption controls.
Its builder sets ORDER_MODE for unordered asynchronous-compute launch; RADV
has a distinct ordered-dispatch path that clears it. Neither choice is a
shader-completion or payload-visibility operation. [PAL direct dispatch][pal-dispatch]
[RADV dispatch policy][mesa-dispatch]

Word 4 of `DISPATCH_DIRECT` carries `COMPUTE_DISPATCH_INITIATOR`. The
ordinary PAL builder initializes it to zero and fills these controls from its
launch arguments and compute-engine policy. Bit positions below follow the
GFX10/GFX11 register definition; the final row is explicitly GFX11.
[Initiator layout][pal-initiator] [Direct-dispatch builder][pal-dispatch]

| Word 4 bit | Field | Value supplied by the ordinary builder |
| --- | --- | --- |
| 0 | `COMPUTE_SHADER_EN` | 1 to enable the compute dispatch. |
| 2 | `FORCE_START_AT_000` | Select zero start coordinates when requested. |
| 5 | `USE_THREAD_DIMENSIONS` | 1 for workitem dimensions; 0 for workgroup counts. |
| 6 | `ORDER_MODE` | 1 for PAL's unordered asynchronous-compute policy. |
| 13 | `TUNNEL_ENABLE` | The caller's tunneling selection. |
| 15 | `CS_W32_EN` | 1 for a wave32 program; 0 for wave64. |
| 17, GFX11 | `DISABLE_DISP_PREMPT_EN` | Set by the builder's disable-partial-preemption request. |

The other modes and reserved bits remain zero in this builder. Tunneling and
preemption choices belong to the native launch policy; neither is derived from
workgroup dimensions. This table describes the cited ordinary path, not every
ordered-append or alternate-engine dispatch mode.

PAL derives SIMD_DEST_CNTL from whether the rounded waves per workgroup are a
multiple of four, with settings that can override the choice. This scheduling
policy is separate from LDS allocation and does not make dispatch packets
synchronous. [Resource-limit derivation][pal-limits]

## Shader wait-counter mode: MEM_ORDERED

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
there. These sources establish the field representation and Mesa's selection
policy; they do not establish the effect of changing the bit on GFX12.
[GFX12 register][pal-gfx12-mem-ordered] [GFX12 wait model][llvm-gfx12-waits]
[Separate wait events][mesa-gfx12-waits]

## Runtime trap and context state

LLVM's GFX6–11 descriptor contract requires zero in compiler-owned storage for
TRAP_PRESENT and certain exception/trap controls; runtime descriptor processing
supplies the live policy. A raw PM4 caller cannot conclude that the live register
must be zero. The libhsakmt raw dispatch builder explicitly sets TRAP_PRESENT
for pre-GFX12 targets. [Descriptor RSRC2][llvm-trap] [Descriptor RSRC3][llvm-prefetch]
[Raw dispatch builder][hsakmt-trap]

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
