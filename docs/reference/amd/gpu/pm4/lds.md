# Compute group memory

Local data share (LDS) is storage private to a workgroup and shared by its
workitems. The compiler describes fixed group requirements and generates LDS
accesses; the launch owner reserves enough storage for fixed and dynamic
regions. A workgroup barrier orders participating waves within that allocation.
A host cache flush or a later dispatch cannot substitute for that local
synchronization. [Group lifetime][llvm-lifetime] [GFX10/GFX11 LDS model][llvm-lds]

## Compiler requirement and launch allocation

An HSA descriptor's LDS register field is deliberately zero. Its fixed group
byte count is separate. Native AQL descriptor processing derives the allocation
from the dispatch packet's total group bytes; a raw PM4 launch owner realizes
the register field itself. This changes live launch state, not the linked
compiler image. [Descriptor contract][llvm-size]

For the GFX11 compute encoding, `COMPUTE_PGM_RSRC2.LDS_SIZE` occupies bits 23:15, in
512-byte units. PAL initializes an HSA pipeline from fixed group bytes and
can override it with the total dispatch allocation. The derived field is:

```text
units = ceil(total_group_bytes / 512)
RSRC2 = (resource_state & ~0x00ff8000) | (units << 15)
```

`resource_state` must already preserve the other compiler/runtime fields.
The total must satisfy the actual device's per-workgroup resource limit;
a nine-bit representation is not a statement of that limit. The cited PAL
device setup reports 64 KiB per workgroup. [Device limits][pal-granularity]
[Fixed-size realization][pal-size] [Dispatch override][pal-dynamic]

Encoding granularity differs from physical allocation. PAL states that
GFX10.3+ physically allocates 1024 bytes for a 512-byte request. Mesa rounds to
1024 bytes before encoding in 512-byte compute units. Thus PAL can encode 1
where Mesa encodes 2 for the same small requirement. These are distinct
runtime policies; neither field value measures occupied bytes or occupancy.
[PAL physical granularity][pal-granularity] [Mesa rounding][mesa-size]

PAL's RESOURCE_LIMITS policy sets SIMD_DEST_CNTL when the rounded wave count
per workgroup is divisible by four, with explicit overrides. That is a
scheduling policy, not part of the LDS byte count or an inter-dispatch fence.
[Wave policy][pal-limits]

## Dynamic regions and argument realization

A dynamic group pointer is a group-segment offset, not a global GPU address.
The compiler's fixed requirement excludes dynamic allocations. Each dynamic
region is aligned after preceding fixed/dynamic bytes, and the total launch
reservation covers every generated LDS access.

PAL's HSA argument realization demonstrates the two representations: its
input argument contains the requested dynamic byte count; the runtime aligns
the current group offset, places that offset into the GPU argument record and
increases the total group requirement. Supplying the byte count directly to a
shader that expects the realized offset would change the ABI.
[Argument realization][pal-arguments]

The executable owner retains the complete code/fetch extent. The argument
owner retains the realized offsets and all fetched argument bytes until the
last dispatch using them completes. Group memory itself has workgroup lifetime;
its initial contents and contents from a preceding dispatch are not input data.
Every value consumed from LDS needs an in-workgroup producer or another
explicitly defined initialization source. [Group lifetime][llvm-lifetime]

## Execution and synchronization

An ordinary cross-wave exchange follows this sequence:

```text
reserve the complete fixed + dynamic group extent
  → bind the realized LDS_SIZE and compatible workgroup/wave geometry
  → each workitem writes its owned LDS locations
  → complete relevant LDS instructions → workgroup barrier
  → workitems read the communicated LDS values → publish global results
```

For GFX10/GFX11, the workgroup is placed within one WGP; LDS is not a cached
global-memory allocation. Compiler wait instructions and a uniform workgroup
barrier supply the cross-wave ordering. Global result publication still needs
the separate shader-completion and memory-visibility protocol described in
[compiled dispatch](dispatch.md#publication-and-shader-dependencies).
[LDS synchronization model][llvm-lds]

## Rebinding resource sizes

PAL's automatic HSA dispatch path retains an LDS override and grows it when
the computed requirement exceeds the bound size. Mesa's variable-size path
re-emits resource state when the program or requested shared size changes,
including smaller requests. Both consume compiler metadata but own their
launch-state policy independently. [PAL retained override][pal-rebind]
[Mesa rebinding][mesa-rebind]

A completed-use CPU rebuild may change the arguments and allocation request
only after prior users have finished. A GPU command stream may bind distinct
immutable programs and resource requests with the dependencies required by its
payload graph. In either case, retaining more physical LDS than a later launch
needs could produce the same output as an exact smaller allocation. Register
programming, correctness of addressed bytes, physical reclamation and occupancy
are different properties.

[llvm-lifetime]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L1181-L1189
[llvm-lds]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L13438-L13464
[llvm-size]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6665-L6688
[pal-size]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputePipeline.cpp#L63-L82
[pal-dynamic]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L838-L856
[pal-granularity]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L5622-L5641
[mesa-size]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_shader_util.h#L352-L365
[pal-limits]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L301-L345
[pal-arguments]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L2457-L2493
[pal-rebind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L772-L807
[mesa-rebind]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L293-L325
