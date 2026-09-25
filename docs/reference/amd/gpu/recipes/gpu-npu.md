# GPU and AI Engine shared-memory handoff

A GPU and an AI Engine array can exchange data through shared external
memory. The GPU accesses that backing through its memory hierarchy; the
array's interface DMA moves data between external memory and streams feeding
local memory. Sharing the allocation establishes neither cache visibility nor
execution order. Input transfer, local computation, output transfer and the
last downstream consumer have separate completion boundaries.

The concrete host flow here uses Linux AMDGPU/KFD and the `amdxdna` AIE2
transport. Descriptor examples use the AI Engine driver's AIE2P shim module.
Native mappings and the selected array configuration supply their respective
address and resource contracts. [XDNA execution](../../xdna/execution.md)
[AIE2P shim selection][shim-module]

## One backing allocation, several address spaces

| View | Meaning |
| --- | --- |
| Exported allocation | Backing held by the exporter and native sharing object. An exported subrange can start at a nonzero offset inside that object. |
| CPU address | A host mapping with its own cache attributes and access protocol. |
| GPU address | A GPU virtual address with permissions, memory type and cache routing selected by its native mapping. |
| NPU address | The native device address for the imported or allocated object, interpreted in the active context's addressing mode. |
| Array-local address | Tile or memory-tile storage reached through the array's local memory/stream interfaces. It is distinct from shared external backing. |

ROCr's portable DMA-BUF export returns both a descriptor and the byte offset
of the requested pointer within the exported object. Its contract explicitly
leaves the imported consistency model to the importer: export from a
fine-grained pool does not guarantee fine-grained shared access through
another API. Export-handle and mapping references preserve backing lifetime;
the application still accounts for outstanding execution.
[Export contract and offset][export]

The XDNA importer takes a DMA-BUF reference, attaches and maps it for
bidirectional DMA, creates an imported GEM object and shares the exporter's
reservation object. `GET_BO_INFO` returns the CPU address, NPU address and
mmap offset separately. The NPU address helper can use a DMA address or a
PASID user virtual address according to the native mapping. Numeric equality
with a GPU or CPU pointer is therefore not an address-translation rule.
[Import owner][import] [Address query][address-query]
[Address selection][address-selection] [PASID and DMA addresses][address-mode]

For an exported subrange, an imported whole-object base plus the returned
export offset identifies the requested range. An address within that range
adds the application's range-relative offset. The original exporting pointer
already identifies the requested range and needs only the range-relative
offset. The importer and exporter retain the full backing extent required by
their native mapping. An address in a descriptor does not itself retain that
mapping or identify all users of it. The ordinary XDNA execution ioctl passes
no argument-BO list to its job constructor; opaque embedded addresses do not
automatically become a lifetime or dependency graph.
[Execution owner](../../xdna/execution.md#linux-context-and-command-representation)

## Visibility at the device boundary

The GPU's release must reach the memory observed by external DMA, and the
GPU's later acquire must see externally written data. The selected GPU
memory type and access path determine whether this requires cache maintenance
or an uncached route. SYSTEM scope within an HSA protocol names that
protocol's participants; it does not independently enroll an NPU importer.
[CPU/GPU mappings](host-device.md#mapping-properties-describe-different-things)
[External consistency][export] [GPU cache protocols](../pm4/cache.md)

XDNA's `SYNC_BO` explicitly maintains CPU-cache visibility for a device that
the driver describes as noncoherent with CPU caches. The backing-specific
implementation flushes a mapped range, imported scatter/gather storage or
pages. This maintenance sits outside normal command submission. It is not
a GPU GL2 writeback, a DMA completion wait or a stop request to a resident
program. The direction-specific debug-buffer work in the ioctl also does
not turn it into a general payload-execution join.
[CPU cache maintenance][cpu-sync]

A complete owner flow consequently separates these operations:

| Edge | Required ordering and visibility |
| --- | --- |
| CPU initialization → GPU/array use | Complete host writes and the native CPU-access/cache-maintenance boundary before publishing executable work or data. |
| GPU payload → NPU external read | Complete the producing shader or DMA operation and its release to the external DMA access path before admitting the NPU read. |
| NPU external write → GPU payload read | Complete the relevant output DMA, establish the dependency, then acquire the GPU cache path used by the consumer. |
| Device output → CPU readback | Observe successful producing work and perform the mapping's CPU-access/invalidation protocol before reading. |

An inactive CPU alias does not perform the devices' handoff. Conversely, a
CPU write to a cache line still owned by external DMA can create a dirty
alias whose later writeback conflicts with device data. Ownership and cache
maintenance therefore cover the actual cache-maintenance granule as well as
the application's useful bytes. [CPU-access ownership](host-device.md#imported-buffers-and-final-use)
[Backing maintenance][cpu-sync]

## A finite GPU → array → GPU sequence

This sequence uses host-observed completions to order the device submissions.
The host transfers control between them while payload remains in shared
backing. The array controller program explicitly drains the workers and DMA
whose result it reports.

1. Establish the exporter, importer, per-device addresses, CPU-access policy
   and allocation extents. Initialize code, descriptors, control storage and
   any CPU-produced payload before either device can fetch them. Retain
   those native mappings for the full execution graph.
2. Publish the GPU producer through its actual
   [PM4](../pm4/publication.md), [AQL](../aql/publication.md), or
   [SDMA](../sdma/publication.md) owner. Its completion includes the producer
   execution join and release required for the NPU's external-memory path.
3. Observe successful GPU completion, then submit the NPU controller program.
   Native submission acceptance supplies no replacement for the preceding
   payload release.
4. The array's MM2S transfers read external input into the stream/local-memory
   graph. Local consumers wait for their destination data, compute, and pass
   results to S2MM output transfers. The controller joins the relevant DMA
   channels and workers before its terminal operation.
5. Wait for the native NPU completion and inspect command status. The driver
   signals terminal errors as well as success; fence readiness alone is not
   a successful-result predicate.
6. Publish the GPU consumer with the acquire operations needed after that
   external producer. Complete its use before reusing output backing.
7. Join remaining command, notification and downstream users before releasing
   code, descriptors, control cells, native mappings and export references.

[Native array completion and drain](../../xdna/execution.md#acceptance-completion-and-result-inspection)
[Array channel join][dma-done] [DMA backend][dma-done-backend]
[GPU release/wait/acquire](../pm4/cache.md#complete-producer-to-consumer-sequence)

A shared DMA-BUF reservation object does not insert these dependencies into
arbitrary user-produced command streams. The GPU queue's synchronization
integration and the NPU submission's actual resource/dependency inputs decide
which native waits exist. The finite flow above supplies explicit ordering
between its completed submissions.
[XDNA import][import] [PM4 transport owners](../pm4/publication.md)

## Array transfers and their local completion

The AIE2P shim module has sixteen 32-byte buffer descriptors, sixteen local
locks and two DMA channels per direction. Its descriptor properties require
four-byte-aligned addresses. `XAie_DmaSetAddrLen` accepts a byte length and
encodes it in DWORD units; the AIE2P shim length offset is zero. These are
descriptor/resource facts, separately from the native context's accessible
address range. [Shim properties][shim-properties] [Shim module][shim-module]
[Address and length construction][dma-length]

AM027 1.1 (October 23, 2025) describes the Versal AIE-ML v2 interface's MM2S
channels as external-read producers feeding streams, and S2MM channels as
stream consumers generating external writes. Its local locks synchronize
descriptor use. The Versal system integration described there does not
define a Ryzen NPU's GPU-cache participation.
[Array interface][array-interface]

| Observation | Resource whose use it describes |
| --- | --- |
| A task-queue slot becomes free | Queue admission capacity. An active transfer can remain. |
| An external-source MM2S transfer completes | Reads performed by that transfer have completed. Repeated or chained tasks can still borrow the same source. |
| A destination-local DMA or lock completes | The specific local buffer edge, according to its programmed producer/consumer protocol. |
| Arithmetic finishes | The local computation's final access, not automatically a downstream stream or external write. |
| Output S2MM channel completes | The native completion of that output channel; later device consumers still own the output. |
| The controller command completes | The work explicitly joined by that controller protocol, with command status inspected. |

These ownership distinctions follow from the separate DMA and execution
actors. A complete external-source read can release its original backing
while downstream computation continues on a retained local copy. For a
broadcast or partitioned transfer, every remaining external reader belongs
to that source's join; completion by one reader releases only its own borrow.
[External and local datapaths][array-interface]
[Tile-local memory and DMA][array-memory]

AIE2P selects the ML implementation of `XAie_DmaWaitForDone`. The normal
channel predicate includes task-queue size, active state, and lock, stream
and token stalls. Its separate pending-descriptor query also accounts for
an active transfer. Queue emptiness alone is therefore insufficient. The
done predicate is not a complete inspection of all sticky DMA error fields.
Its timeout argument is microseconds; zero selects the API's default timeout,
unlike the separate native XDNA host wait whose zero means infinite.
[Backend selection][shim-module] [Done API][dma-done]
[Done predicate][dma-done-backend] [Pending count][dma-pending]
[Native wait](../../xdna/execution.md#acceptance-completion-and-result-inspection)

## Resident programs and per-generation ownership

When the devices remain active across many records, dispatch-end release and
the next dispatch's acquire cannot carry each record's handoff. Each record
needs its own control and payload visibility edge inside the running program.
Three progress values answer different questions:

| Progress | Meaning assigned by the protocol |
| --- | --- |
| Input ready | All contributors have published this generation of external input for the NPU readers. |
| Source consumed | Every external read of that generation has completed, permitting source-slot overwrite. Local copies may remain in use. |
| Output ready | All contributing external output writes have completed and are published for the GPU consumer. |

The protocol also defines each value's writer, admitted access width,
alignment, reader path and generation arithmetic. Reading one DWORD of an
eight-byte cell establishes only the chosen DWORD protocol, not a general
64-bit atomic timeline. A comparison using ordinary unsigned ordering needs
an interval excluding generation wrap, or a different explicitly bounded
modular comparison. Storage remains live through its final waiter.
[Control and reuse](README.md#control-signals-and-reuse)

### Publication inside a GPU shader

LLVM's GFX10/GFX11 memory model separates per-CU vector L0, shader-array L1
and agent L2. L2 coherence with external agents depends on the target and
mapping; a mapped bypass route is another possibility. VMEM completion is
reported to the issuing wave. A barrier among waves does not replace their
required memory completion operations. [Shader hierarchy and ordering][shader-model]

For a cooperating workgroup publishing one generation, the ownership
composition is: each contributing wave completes the required payload
stores/release, the waves rendezvous, and the designated publisher writes
the control value. A workgroup rendezvous does not join other workgroups;
their contributions need an additional explicit join. The mapping must
already supply the external observer's required cache behavior. An uncached
payload route removes a cache obligation only for that route; it does not
remove store completion or the contributor join.
[Wave completion][shader-model] [Workgroup barriers][workgroup-barrier]

The consumer similarly distinguishes a fresh control load from payload
acquisition. LLVM's GFX10/GFX11 sequence for a GLOBAL atomic acquire load at
AGENT or SYSTEM scope waits for the buffer/global load, then invalidates
GL1/GL0 before subsequent payload loads. GFX11 uses `glc=1` for that load;
GFX10 additionally uses `dlc=1`. The sequence assumes the surrounding memory
model's admitted atomic access and valid L2 route. A fresh control word cannot
invalidate independently cached payload by itself.
[Acquire sequence][shader-acquire]

Scalar loads have a different compiler premise: the ordinary scalar path
assumes the memory remains unchanged during the dispatch. A mutable
resident control queue cannot inherit that invariant merely because its
address is uniform across lanes. [Scalar-memory premise][shader-model]

### Output DMA and a ready flag

AIE2P descriptor word 7 contains separate local lock acquire and release
operands. A payload descriptor can release a shim lock and another
descriptor can acquire it before executing a flag transfer. This expresses
a local dependency between descriptors; the lock is not an atomic operation
on a GPU-visible memory cell. [Lock fields][shim-locks]
[Descriptor construction][shim-lock-writer]

Using that second channel's flag as GPU-visible release publication requires
two additional facts: the payload lock release follows the required external
write completions, and the flag reaches the common memory observer after
those writes. The cited AIE2P implementation defines the descriptor fields
and ordinary channel-done predicate, but does not specify that stronger
cross-channel external-ordering rule. The missing contract is the lock-release
point relative to outstanding write responses and the ordering from there
to the second channel's flag. An applicable array/fabric completion rule is
needed to establish that composition.
[Local lock representation][shim-locks] [Channel predicate][dma-done-backend]
[External interface][array-interface]

`AXCACHE` supplies descriptor-level AXI attributes; it does not by itself
establish this ordering or GPU coherence. The driver's separate
`XAie_DmaSetAxi_AxUser` interface with an `IOCoherence` operand explicitly
requires AIE4. Its presence is not an AIE2P coherency guarantee.
[AXI attributes][axi-attributes] [AIE4 applicability][axi-user]

## Final drain and reuse

Stopping a resident exchange closes its admission and then satisfies already
admitted dependencies. A worker waiting for a stream, local semaphore or
external generation needs the corresponding wake/termination protocol.
Changing an unrelated stop cell does not finish that wait.
[Program quiescence](../../xdna/execution.md#placement-and-program-quiescence)

The final join covers GPU contributors, NPU external reads, local buffer
users, output DMA, GPU output consumers and any later notification commands.
Descriptors and code remain valid until their last fetch/execution, while
payload slots can have earlier per-generation reuse points. Native queue or
context removal is a separate owner operation. A terminal error or host wait
timeout requires the native failure/termination path; it is not evidence
that those users have released their memory.
[Command ownership](../pm4/publication.md#storage-reuse-boundaries)
[Transfer final users](../sdma/publication.md#completion-and-storage-ownership)
[Native result and timeout](../../xdna/execution.md#acceptance-completion-and-result-inspection)

[export]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L4425-L4473
[import]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L999-L1033
[address-query]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1369-L1399
[address-selection]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L274-L280
[address-mode]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.h#L91-L117
[cpu-sync]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1402-L1504
[shim-properties]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L2062-L2085
[shim-module]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L2151-L2190
[dma-length]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c#L564-L616
[dma-done]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c#L2177-L2231
[dma-done-backend]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L2096-L2160
[dma-pending]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L2001-L2046
[shim-locks]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L1972-L1989
[shim-lock-writer]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L1587-L1660
[axi-attributes]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c#L1166-L1215
[axi-user]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c#L1217-L1260
[array-interface]: https://docs.amd.com/r/en-US/am027-versal-aie-ml-v2/AIE-ML-v2-Array-Interface
[array-memory]: https://docs.amd.com/r/en-US/am027-versal-aie-ml-v2/AIE-ML-v2-Memory-Module
[shader-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L13564-L13695
[shader-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L13905-L13932
[workgroup-barrier]: ../pm4/lds.md#execution-and-synchronization
