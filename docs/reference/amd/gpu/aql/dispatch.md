# AQL kernel dispatch

An AQL kernel-dispatch packet names an AMDHSA kernel descriptor, argument
storage, a grid, and per-dispatch resource requirements. The packet processor
combines the compiler descriptor with native queue state to launch workgroups.
Descriptor fields, executable instructions, and firmware launch policy have
different owners. [HSA packet][hsa-geometry] · [LLVM dispatch
ABI][llvm-dispatch]

## Packet and executable representation

The dispatch packet occupies 64 bytes. Its header uses kernel-dispatch type 2;
the [barrier and fence fields](barriers.md#header-and-barrier-representation)
control packet ordering and memory visibility.

| Byte offset | Field and unit |
| --- | --- |
| 0 | 16-bit header. |
| 2 | 16-bit setup; bits 1:0 encode dimension count 1, 2, or 3. |
| 4, 6, 8 | 16-bit nominal workgroup sizes X, Y, Z, in workitems. |
| 10 | Reserved 16 bits. |
| 12, 16, 20 | 32-bit grid sizes X, Y, Z, in workitems. |
| 24 | 32-bit private-segment bytes per workitem. |
| 28 | 32-bit total group-segment bytes per workgroup. |
| 32 | 64-bit kernel object, identifying the descriptor. |
| 40 | Kernarg address in the large-machine-model layout. |
| 48 | Reserved 64 bits. |
| 56 | Completion signal handle, or zero. |

Unused dimensions have workgroup and grid size one. Each grid axis is positive
and at least its nominal workgroup size. Resource totals include the kernel's
requirements and those of functions it calls. [Packet definition][packet]

The LLVM descriptor at the cited revision is 64 bytes, aligned to 64 bytes.
Its signed entry offset leads from the descriptor base to code aligned to 256
bytes. Kernargs have at least 16-byte alignment and satisfy any larger
alignment required by the compiled object. [Descriptor and argument
alignment][llvm-layout]

| Descriptor byte offset | Compiler-owned field |
| --- | --- |
| 0, 4, 8 | Fixed group bytes, fixed private bytes per workitem, kernarg bytes. |
| 16 | Signed 64-bit code-entry offset from the descriptor. |
| 44, 48, 52 | Target-specific `COMPUTE_PGM_RSRC3`, `RSRC1`, `RSRC2` encodings. |
| 56 | Kernel code properties, including initial user-register enables. |
| 58 | Kernarg preload specification. |
| 12–15, 24–43, 60–63 | Reserved bytes in this descriptor version. |

[LLVM descriptor declaration][llvm-descriptor]

ROCr's copied loader header at the cited revision still calls bytes 58–63
reserved. LLVM defines preload length and offset in DWORDs. For a nonzero
preload length, supporting firmware advances the entry by 256 bytes to skip
the compatibility prologue. Thus the descriptor version, compiler prologue,
and firmware capability must agree; the copied runtime structure alone does
not describe every supported code-object feature. [ROCr descriptor
copy][rocr-descriptor] · [LLVM preload protocol][llvm-preload]

## Arguments, geometry, and initial registers

Argument layout comes from the executable ABI, rather than the host compiler's
incidental structure padding. Accessible backing covers the actual
compiler-generated fetches, which can extend beyond the last semantic member.
Initializing that padding supplies defined read contents; it does not add
arguments to the ABI. Argument storage is published before the packet and
stays unchanged through dispatch completion. [LLVM argument
ABI][llvm-dispatch] · [HSA §3.3.3.1][hsa]

The nominal workgroup product obeys the compiler's flat-workgroup-size
attribute. That attribute and the uniform-workgroup property are separate
contracts: an allowed nominal size does not imply that a partial final group
is permitted by a uniformly compiled kernel. HSA defines partial final groups,
while the compiler metadata records whether every group must be full. [LLVM
workgroup attribute][llvm-flat-size] · [Uniform metadata][llvm-uniform] · [HSA
partial groups][hsa-partial]

Group IDs and local workitem IDs are initialized according to enabled
descriptor fields and the target ABI. The user SGPR sequence precedes enabled
system SGPR inputs; local IDs use target-specific VGPR representations.
Separate X/Y/Z inputs and packed representations are not interchangeable.
Consumers derive their bindings from the exact target descriptor and entry,
rather than assuming one register convention for all AMD GPUs. [Initial
registers][llvm-initial-ids] · [Packed workitem IDs][llvm-packed-ids]

For a partial grid, inactive lanes in the last group are not valid workitems.
A kernel's explicit element-count guard is a separate algorithmic bound: a
full grid can launch extra workitems that return before touching payload.
Workgroup barriers and local-memory exchanges still obey the compiled
workgroup contract. [HSA partial groups][hsa-partial] · [Compiler
uniformity][llvm-uniform]

Unaligned global accesses also have a target contract. LLVM permits GLOBAL
misaligned accesses when the target has unaligned-buffer-access support and
the corresponding mode is enabled; the HSA subtarget enables that mode.
Linux's V9 queue setup supplies the related native memory configuration. This
explains why an `align 1` source access can lower to a multi-DWORD GLOBAL
instruction on gfx942. It does not waive kernarg, descriptor, atomic, or
different-address-space alignment requirements. [GLOBAL lowering
predicate][unaligned-global] · [Feature and mode][unaligned-enabled] · [HSA
mode][unaligned-hsa] · [Native V9 configuration][unaligned-kfd]

The target-specific link is explicit: the gfx942 processor selects
`FeatureISAVersion9_4_2`, whose inherited GFX9 feature set includes unaligned
buffer/global access. This compiler capability and the native alignment mode
are complementary premises, not a claim that an arbitrary queue configuration
accepts every unaligned instruction family. [Processor][gfx942-processor] ·
[ISA feature set][gfx942-features] · [Common GFX9 inheritance][gfx94-features]
· [Inherited capability][gfx9-features]

## Private storage

Private storage belongs to individual workitems and can include explicit
private variables, spills, and call-frame requirements. The compiler's fixed
private byte requirement need not equal the source-level variable total. On
gfx942, architected flat-scratch initialization obtains `FLAT_SCRATCH` from
queue backing and a physical-wave offset; legacy private-buffer and
flat-scratch-init user-SGPR inputs are not substitutes for that mechanism.
[Private address space][llvm-private] · [Architected
initialization][llvm-scratch-init]

ROCr distinguishes retained scratch from single-dispatch scratch. Retained
scratch must cover every physical scratch slot, even when one dispatch has
fewer waves. Its single-use path can size backing for the dispatch instead,
because firmware surrenders that allocation under a separate reclamation
protocol. The per-XCC descriptor uses that XCC's share of the backing.
[Allocation and retention policy][rocr-retained-scratch] · [Per-XCC descriptor
construction][rocr-scratch-xcc]

A fixed-scratch queue therefore keeps its backing alive while the queue can
reuse it. Completion of a small dispatch does not establish that every
physical slot was used, nor does it transfer queue-owned scratch to the host.
Dynamic growth, single-use reclamation, and queue teardown have their own
ownership transitions in the native runtime. [ROCr scratch ownership
distinction][rocr-retained-scratch]

## Static and dynamic group storage

Group memory is shared within one workgroup. A group pointer is a
group-segment offset, not a global virtual address. Static storage contributes
the fixed descriptor requirement; the dispatch packet supplies the total
allocation, including dynamic group storage and called functions. The packet
total must be at least the fixed requirement. [Group address
space][llvm-group-space] · [Packet total][hsa-group-size]

The command processor derives the hardware LDS-size field from that total,
rounded to the target granule; the corresponding descriptor LDS-size bits are
zero in this ABI. For gfx942 the granule is 512 bytes. A dynamic array
following 512 static bytes can therefore begin at group offset 512, with its
bounds contained in the requested total. Offsets and strides are arguments;
they do not change the descriptor's fixed requirement. [LDS initialization and
granularity][llvm-lds-size]

Cross-wave exchange requires local stores to become visible before partner
loads. CLR's local-memory synchronization combines release, a workgroup
barrier, and acquire; emitted wait and barrier instructions must match the
target's LDS ordering. Every read needs an initialized writer within the same
workgroup. Changing dispatch totals or switching immutable kernels requests
different resources, but does not by itself specify physical clearing of
scratch or LDS contents. [Local-memory barrier][clr-local-barrier] · [Packet
resource totals][packet]

## Executable publication and final use

Executable publication has three obligations: finish and publish image writes,
complete executable-cache maintenance, and retain code until all its users
finish. An address-range invalidation does not discover other queues still
executing an old image at that address.

ROCr's base-profile loader stages the image separately from its final local
allocation. Freeze selects a blocking upload when Large BAR is unavailable or
the image exceeds the configured threshold; otherwise it writes the mapped
destination and publishes PCIe writes. The blocking upload here selects a
utility-queue shader copy and acquires its completion. This is a shader-copy
upload path, not evidence of an SDMA upload sequence.
[Staging][loader-backing] · [Upload selection][freeze] · [Copy
selection][loader-blit] · [Copy completion][loader-copy-wait] · [PCIe
publication][loader-pcie]

After successful upload, `InvalidateCodeCaches` synchronously submits an
ACQUIRE_MEM indirect buffer on the utility AQL queue. Its pre-GFX10 branch
uses seven DWORDs with instruction/scalar invalidation and TC
invalidation/writeback; its GFX10–GFX12 branch uses eight DWORDs with
GLI/GLK/GLV/GL1/GL2 invalidation. The range is expressed in 256-byte units.
The source also has early-microcode and explicit runtime-mode exceptions;
those are predicates of this caller, not general permission to omit
publication. [Cache builder and predicates][code-cache]

The synchronous carrier uses NONE/NONE scopes and a clear header barrier, then
waits for native completion while retaining its shared IB. The explicit cache
command supplies this publication operation. Ordinary SYSTEM data fences and
ring consumption do not replace it. [Carrier publication and
completion][loader-cache-carrier]

HSA's executable-handle lifecycle is load, freeze, immutable use, then destroy
after the caller completes outstanding kernels. Destruction does not perform
that kernel join. At the cited ROCr revision, executable freeze discards a
segment freeze result before marking the executable frozen, although segment
upload can fail. The successful-path sequence is visible in the source; that
failure path prevents returned freeze success from independently proving a
successful upload. [Freeze contract][hsa-freeze] · [Destruction
precondition][hsa-destroy] · [Segment result][freeze] · [Result
handling][loader-freeze]

### Reusing a completed code address

Caller-owned raw code storage has a distinct lifecycle from a frozen HSA
executable handle. A completed-use replacement composes the following native
obligations:

1. Join every old dispatch and indirect command that can access the code.
2. Retire the naming packet slots before rewriting their ring storage. Retain
   code backing and mappings while replacing the complete image, including
   compiler-required padding and any fetchable tail.
3. Publish the replacement writes for the actual mapping and complete the
   target's executable-cache operation.
4. Only then publish the next dispatch with its matching descriptor, argument
   ABI, and resource requirements.

This is a composition of last-use ownership and the source-visible publication
path. Cache invalidation is not an old-user join, and the sequence does not
create a mutable or refreezable HSA executable handle. [HSA last-use
requirement][hsa-destroy] · [Executable publication][code-cache] ·
[Synchronous native completion][loader-cache-carrier]

## PM4 shader state inside an AQL queue

An AMD vendor-format-1 packet can carry a PM4 indirect buffer. ROCr's
virtual-XCC0 predicate for globally visible memory operations does not specify
how shader workgroups are distributed: its rationale explicitly distinguishes
memory addresses from execution units. [Memory-operation
routing][rocr-memory-routing]

KFD initializes GC9.4.3 AQL queues with `COMPUTE_TG_CHUNK_SIZE = 1` and a
rotating `COMPUTE_CURRENT_LOGIC_XCC_ID`. Raw PM4 queues receive zero for both,
with separate PM4 target-XCC selection. The update path preserves a format
distinction. Mesa's ordinary CDNA preamble writes chunk size zero for GFX940
and newer compute families in its own command context. [KFD
initialization][linux-xcc-initialize] · [KFD update][linux-xcc-update] · [Mesa
preamble][mesa-cdna-preamble]

These sources establish different initial conditions. They do not establish
the vendor firmware's complete shader distribution and state-restoration
contract when returning to ordinary AQL dispatch. Compiler resource words also
do not supply all live launch policy, including the native trap-handler
setting; see [PM4 dispatch](../pm4/dispatch.md). A shader-bearing carrier
therefore needs those additional native contracts. Confirmed memory
operations, shader completion, and whole-IB completion remain different
mechanisms. [Descriptor dispatch model][llvm-dispatch] · [Native queue
state][linux-xcc-initialize]

Return to [AQL](README.md) or [barriers and signals](barriers.md).

[hsa-geometry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2952-L3037
[llvm-dispatch]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L5963-L6025
[packet]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2952-L3090
[llvm-layout]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L6138-L6226
[llvm-descriptor]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/include/llvm/Support/AMDHSAKernelDescriptor.h#L266-L330
[rocr-descriptor]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/loader/AMDHSAKernelDescriptor.h#L214-L244
[llvm-preload]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7229-L7258
[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[llvm-flat-size]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L2597-L2607
[llvm-uniform]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L5824-L5833
[hsa-partial]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-PRM-1.2.pdf#page=26
[llvm-initial-ids]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7044-L7125
[llvm-packed-ids]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7142-L7198
[unaligned-global]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/SIISelLowering.cpp#L2276-L2283
[unaligned-enabled]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/GCNSubtarget.h#L334-L335
[unaligned-hsa]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/GCNSubtarget.cpp#L108-L111
[unaligned-kfd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager_v9.c#L59-L109
[gfx942-processor]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/GCNProcessors.td#L195-L197
[gfx942-features]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/AMDGPU.td#L1890-L1898
[gfx94-features]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/AMDGPU.td#L1828-L1829
[gfx9-features]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/lib/Target/AMDGPU/AMDGPU.td#L1520-L1534
[llvm-private]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L6068-L6106
[llvm-scratch-init]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7407-L7424
[rocr-retained-scratch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2897-L2943
[rocr-scratch-xcc]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1802-L1811
[llvm-group-space]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L6038-L6066
[hsa-group-size]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L3044-L3050
[llvm-lds-size]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L6791-L6810
[clr-local-barrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/hipamd/include/hip/amd_detail/amd_device_functions.h#L697-L711
[loader-backing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_loader_context.cpp#L281-L345
[freeze]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_loader_context.cpp#L347-L373
[loader-blit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1037-L1055
[loader-copy-wait]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L612-L640
[loader-pcie]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_agent.h#L474-L483
[code-cache]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3437-L3497
[loader-cache-carrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1758
[hsa-freeze]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L4444-L4475
[hsa-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L4292-L4314
[loader-freeze]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/loader/executable.cpp#L2246-L2266
[rocr-memory-routing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4761-L4775
[linux-xcc-initialize]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L727-L785
[linux-xcc-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L799-L830
[mesa-cdna-preamble]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf.c#L128-L150
