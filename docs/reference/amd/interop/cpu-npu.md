# CPU and NPU memory handoff

A CPU passes external-memory input to an NPU by publishing the bytes before
admitting the array's reads. The return path completes the array's output
writes before making those bytes current in the CPU mapping. Allocation,
cache maintenance, execution completion and final storage use each provide a
different part of this handoff. Array-local buffers and locks carry the
intermediate dataflow; they do not replace the external-memory protocol.

## Native mapping and address spaces

The Linux flow here uses the `amdxdna` AIE2 execution transport and its XRT
KMQ shim. That shim reports a noncoherent CPU/device cache relationship.
Its ordinary host allocations are shared external backing, reached by array
interface DMA through the native context. The [tile DMA chapter](../xdna/dma.md)
describes AIE2P descriptors and local transfers; [native execution](../xdna/execution.md)
describes context creation, instruction submission and firmware completion.
The separately described Versal GMIO flow has a different host integration.
[KMQ memory policy][kmq-policy] [Native cache contract][driver-sync]

| Native storage kind | Backing and address interpretation |
| --- | --- |
| `AMDXDNA_BO_SHARE` (`SHMEM` is its legacy spelling) | Owned shared storage, registered user memory, or imported backing. The creation path determines which. |
| `AMDXDNA_BO_DEV_HEAP` | A native-addressed heap backed by the driver's shared-memory allocation path. It is not tile SRAM. |
| `AMDXDNA_BO_DEV` | A page-rounded range allocated from that heap. Its native address and CPU heap view name the same selected range. |
| `AMDXDNA_BO_CMD` | Shared backing marked internal for command storage. Command ownership is separate from indirectly addressed instructions and payload. |

[BO kinds][bo-kinds] [Shared-allocation selection][share-create]
[Heap creation][heap-create] [Heap suballocation][dev-create]
[CPU heap view][heap-view]

In the ordinary non-carveout, non-`AMDXDNA_NPU3A` owned path, the driver uses
GEM shmem and sets `map_wc=false`. The Linux shmem mmap helper preserves the
normal VMA protection instead of applying write-combining. This describes
that CPU mapping; it does not make the NPU coherent with CPU caches. The
carveout, CMA, registered-memory and imported paths have their own backing
owners. [Owned shmem mapping][shmem-create] [Linux mmap policy][shmem-mmap]
[Allocation predicates][share-create]

`GET_BO_INFO` returns a CPU virtual address, an NPU address and a DRM mmap
offset separately. The mmap offset selects a mapping; it is not a DMA
address. For ordinary shared objects, the address helper chooses an installed
DMA address when present and otherwise the user virtual address used by the
PASID path. Heap objects have their separate native base and suballocation
addresses. Even when the numbers coincide, that coincidence does not permit
substituting an arbitrary CPU pointer for the queried NPU address.
[Address query][bo-info] [Native address selection][address-selection]
[PASID/DMA address choice][address-mode]

Registration also has a representation boundary. The `CREATE_BO.vaddr`
comment in the cited UAPI calls it a user VA and says it must be zero, but the
implementation's nonzero path treats it as a pointer to an
`amdxdna_drm_va_tbl`. The actual XRT caller constructs that table, containing
one page-aligned user range with a page-rounded length. The driver accepts
page-aligned, nonempty entries, accounts pinned memory, and requires the
entries to form one contiguous user-VA range when an IOVA domain is absent.
With an IOVA domain, scattered entries can form one contiguous device range.
This table-based caller and implementation establish the registered-memory
flow at this revision. [Creation representation][create-uapi]
[Table interpretation][user-create] [Registration caller][user-caller]
[Entry validation and pin accounting][user-ranges]

## CPU cache maintenance and its real extent

XRT's ordinary BO `sync` delegates to the selected buffer transport. In the
cited KMQ shim, `buffer::sync` normally runs a CPU cache operation directly;
`Debug.force_driver_sync` instead selects the `SYNC_BO` ioctl. On x86 the
direct path issues `CLFLUSH` for the touched cache lines, with `MFENCE` before
and after. Its line size comes from `_SC_LEVEL1_DCACHE_LINESIZE`. Both
TO_DEVICE and FROM_DEVICE use this same direct operation. Direction names
describe the ownership transfer, rather than selecting two different
instructions in this implementation. [BO delegation][bo-sync]
[Shim selection and bounds][shim-sync] [Direct cache operation][direct-flush]
[Line-size source][line-size] [Driver-sync selection][sync-selection]

For CPU input, the cache operation makes completed host writes available
before the device reads. For output, it makes the CPU reload device-produced
bytes after the producer has completed. The host must have relinquished dirty
aliases before the NPU writes: a later clean-and-invalidate operation can
write back a dirty CPU line as well as invalidate it. The shim explicitly
flushes newly allocated shared storage before returning it because dirty
pages could otherwise write back over a subsequent NPU result. Merely
leaving the output array unused in source code does not establish this
initial clean state. [Initial shared-buffer flush][initial-flush]

`SYNC_BO` carries a 32-bit BO handle and direction (`TO_DEVICE=0`,
`FROM_DEVICE=1`), followed by 64-bit byte offset and byte size. The shim
rejects ranges extending outside its BO. The driver's ordinary backing
flush helper rejects an offset at or beyond the object end and an overflowing
`offset+size`, then clips the end to the object. The `DEV` path first
intersects the requested native-address range with its underlying heaps and
flushes each intersection. Those are distinct layers of range handling.
[Sync representation][sync-uapi] [Shim bounds][shim-sync]
[Backing flush][flush-helper] [Heap maintenance][driver-sync]

The driver chooses its maintenance implementation from the available backing:

| Selected path | Storage covered by CPU maintenance |
| --- | --- |
| Direct XRT shim, ordinary nonempty range | CPU cache lines touched by the requested mapped byte range. |
| Successful driver `vmap` | `drm_clflush_virt_range(kva+offset, size)`; on x86 with CLFLUSH, the intersecting CPU cache lines. |
| Imported object without that virtual mapping | `drm_clflush_sg` over the imported scatter/gather table, including every represented page. The logical subrange does not restrict this fallback. |
| Owned page array without that virtual mapping | Whole pages from `floor(offset/PAGE_SIZE)` through the rounded-up end of the range. |

[Backing selection][flush-helper] [Virtual-range operation][drm-range]
[Page and scatter/gather operations][drm-pages]

The ioctl performs the same ordinary payload flush for either direction.
FROM_DEVICE additionally invokes the driver's debug-buffer synchronization
helper; this does not make an ordinary payload sync a DMA wait or resident
program stop. Submission deliberately leaves payload cache maintenance to
this separate owner. A CPU fence orders CPU operations; it does not cause an
outstanding NPU write to complete. [Native sync implementation][driver-sync]

### Independent slots and aliases

The maintenance extent is part of a slot's ownership contract. Two logical
slots on different cache lines can still share a page covered by the page
fallback, or the entire imported object covered by the SG fallback. Before a
CPU performs maintenance, every affected range must be compatible with that
operation. Concurrent CPU writers can create dirty lines outside the intended
slot; flushing those lines while the NPU writes can overwrite device data.
Repeatedly invalidating a control cell does not isolate that cell when the
chosen backend maintains more storage. This follows from the actual ranges
in the table, rather than from a universal cache-line size.
[Backing flush selection][flush-helper] [Linux cache operations][drm-pages]

Shared, clean, read-only lines are a different case: they do not present the
same dirty-writeback conflict. The problem is overlapping ownership and
maintenance, not the existence of a second address. A resident protocol
therefore fixes both its maintenance path and the range over which writers
are excluded, or coordinates all owners of the larger affected extent.
Separating allocations can establish a useful boundary when subranges cannot.

## Publishing commands, instructions and payload

These three storage classes have different consumers. In the ordinary
instruction-buffer IRON/XRT flow, the host publishes input and instruction
BOs, binds the input/output objects, and invokes the kernel with an opcode,
instruction BO and instruction byte count. The host runtime retains the
instruction BO across the run wait. Its tensor transport initializes mapped
bytes and synchronizes them when creating an NPU-resident tensor, including
the instruction tensor. [Tensor initialization][tensor-init]
[Invocation and instruction owner][host-run]

The native command envelope carries an instruction address and byte size;
copying that envelope does not copy the instruction stream. With the AIE2
command-list path selected, the driver copies the envelope into its own
slot, flushes that slot, then sends the firmware mailbox request. This
publishes the driver-owned command list. It does not flush arbitrary payload
addresses embedded in the program. [Slot construction][command-slot]
[Command-list publication][command-publish]

The shim's submission structure can include argument BO handles, but the
cited ordinary execution ioctl calls its job constructor with no argument BO
list. Driver retention of the command object therefore cannot substitute for
caller retention of all instructions, arguments and reachable data. The
complete caller keeps those owners valid through their final device users.
[Shim submission][shim-submit] [Ordinary ioctl][exec-ioctl]
[Explicit argument retention mechanism][argument-owner]

## A finite CPU → NPU → CPU flow

The IRON SAXPY caller supplies two 4096-element `bfloat16` inputs and one
output: 8192 bytes per tensor. Its array worker acquires X, Y and a free Z
object, computes `3*X+Y`, then releases all three. The runtime emits two input
fills and an output drain with `wait=True`. This is a concrete dependency
chain from CPU-produced input through array-local computation to external
output. [SAXPY dataflow][saxpy] [Host allocation and use][saxpy-host]

1. **Create and initialize the owners.** Load the program into its native
   context and allocate the instruction, input and output BOs. Obtain their
   NPU addresses through the selected transport. Initialize the input bytes
   and establish clean CPU aliases for output. The tensor constructor copies
   or zeroes its mapped bytes and performs the NPU publication operation.
   The ordinary dispatch path also moves any host-dirty argument ranges to
   the NPU before extracting BO handles. [Tensor initialization][tensor-init]
   [Range publication][tensor-reconcile] [Host dispatch][host-run]
2. **Install and start the transfers.** The generated controller program
   writes the shim BDs, patches each external address from its runtime
   argument, and publishes the DMA tasks. `DMATask.resolve` turns the output
   wait request into token production on the output task. Complete native
   descriptor construction and the task token are described in
   [tile DMA ownership](../xdna/dma.md#one-complete-iron-transfer-flow).
   [Task resolution][task-resolve]
3. **Pass local buffer ownership through the array.** The two MM2S inputs
   feed local objects before the worker acquires them. The worker releases
   the produced Z object to its S2MM consumer. In this finite graph, the
   output depends on both input reads; an unrelated output would not retire
   them. [SAXPY worker and sequence][saxpy]
4. **Await output before recycling transfer resources.** The output await
   becomes the selected shim task's completion-token wait. Runtime group
   finalization emits waits before frees. A free returns compiler-managed
   BD resources; it is not a second device wait. The dependency in step 3
   supplies the last-read boundary for the unawaited inputs.
   [Task-group ordering][task-group]
5. **Observe native success.** The host runtime waits on the XRT run handle
   and, in its ordinary error-checking path, requires
   `ERT_CMD_STATE_COMPLETED`. The driver's notification fence is also signaled
   on terminal errors, so fence readiness alone is insufficient. Instruction
   and payload owners remain live through the wait.
   [Host result check][host-run] [Firmware response handling][responses]
   [Fence notification][notify]
6. **Acquire and consume the output.** Only after successful producing work,
   perform FROM_DEVICE for the output's CPU-access range. The caller's
   `numpy()` read reconciles device-owned ranges through this operation
   before returning the host view. Keep output storage until the CPU's final
   read, and through any additional downstream device consumer.
   [Tensor readback][tensor-read] [Range reconciliation][tensor-reconcile]
   [XRT transport synchronization][tensor-sync]

The same backing can be reused for another finite invocation after these
last-use boundaries, with fresh CPU writes published before the next device
access. In-place input/output additionally requires that all input reads of
the overwritten range complete before any conflicting writer starts.

## Final use, registered memory and imports

| Resource | Last use that matters |
| --- | --- |
| External input range | The final MM2S or other device read of that range, joined directly or through a complete dependent result. |
| External output range | Producing S2MM completion, followed by every CPU or device consumer's final read. |
| Command BO and instruction bytes | Their native command users, including every indirect fetch; envelope copying releases neither automatically. |
| Registered user range | All device users, the registration/mapping lifetime, and the application's original backing owner. |
| Imported DMA-BUF | Every importer and exporter execution user, plus the references and attachment mappings holding the backing. |
| Local buffers, BDs, locks and routes | Every worker or queued, active or repeated DMA operation able to revisit them. |

The imported-object path retains the DMA-BUF, creates a bidirectional DMA
attachment mapping and shares the exporter's reservation object. Its free
path unmaps the attachment, detaches and drops the DMA-BUF reference. Those
operations preserve backing ownership while it is imported; they are not an
automatic dependency for opaque command-stream addresses. CPU access to an
import also retains the exporter's native CPU-access requirements, as
explained in [imported-buffer ownership](../gpu/recipes/host-device.md#imported-buffers-and-final-use).
[Import construction][import-owner] [Import release][import-release]

Ordinary user registration pins its pages with `FOLL_LONGTERM`, adds write
pinning when appropriate, and installs memory-invalidation tracking. Pinning
and invalidation handling do not authorize the application to repurpose a
live range or remove the original mapping while the submitted program can
still address it. The registered page-rounded extent must belong to the
caller even when its useful payload is smaller. [Page acquisition][user-pin]
[Registration and HMM setup][user-ranges] [Invalidation owner][hmm-owner]

The default IRON worker repeats its body in a `range_(sys.maxsize)` loop.
Finishing one SAXPY output therefore establishes the finite external-data
boundary described above, while the worker and its local program can remain
resident. Replacing local code, descriptors, routes or lock state requires
separately quiescing every user of those resources. A host wait timeout is
neither cancellation nor that quiescence; error handling follows the native
command and recovery owner before permitting reuse.
[Worker default][worker-default] [Worker loop lowering][worker-loop]
[Native completion and recovery](../xdna/execution.md#acceptance-completion-and-result-inspection)

## Resident input, credits and output

A resident program moves the ownership transfer inside one invocation.
Each generation needs an explicit input-ready event, a last-reader credit
for input reuse, and output completion before host consumption. Native
submission acceptance cannot supply these per-generation edges. Array-local
locks synchronize their named local participants; an external ready cell
also needs an admitted access width, address path and ordering relationship
to its payload. The [resident publication discussion](../gpu/recipes/gpu-npu.md#output-dma-and-a-ready-flag)
separates an S2MM/local-lock event from publication to an external observer.

For CPU input, the owner finishes its writes and the selected maintenance
operation before publishing the generation by a control path the resident
reader can observe. The reader's acknowledgement must cover all external
reads of that generation, including repeated DMA and multiple recipients.
Local computation may continue on retained local copies after that input
credit is returned. For CPU output, the protocol first establishes completion
of all contributing external writes, then the CPU refreshes its payload
mapping before reading. A fresh ready value does not perform the payload's
cache operation. [Local and external ownership](../xdna/dma.md#final-use-and-architecture-boundaries)

For a reusable output slot, the return edge runs from the CPU back to the
array. After the last CPU or downstream reader finishes, the CPU publishes
an output-consumed credit through the admitted, ordered control path,
including any cache maintenance that path needs. The array-side owner waits
for and acquires that credit before allowing the next S2MM overwrite. An
external credit word does not become a tile-local lock automatically; the
controller or dataflow protocol connects those two ownership domains. This
is the required protocol composition, rather than a definition of an
otherwise unspecified array polling instruction.

If the resident program publishes a ready flag through another DMA, the
protocol must establish the payload-to-flag ordering on that native external
path. A local release or channel ordering statement supplies only the edge
it defines; neither CPU polling nor a later flush repairs a missing producer
edge. Conversely, a supported completion wait for the producing task gives a
finite path without assuming that an earlier per-record flag has the same
meaning. The CPU's control-cell maintenance is subject to the page/full-SG
extent rules above, as is payload readback. Slots and credits remain live
through their final waiters, and stopping the producer alone does not join
pending readers or DMA.

## Managed Versal GMIO

UG1603 revision 2026.1 (July 8, 2026) describes a complete processing-system
(PS) path using `GMIO::malloc`, non-cacheable Linux mappings, input
`gm2aie_nb`, output `aie2gm_nb`, and `gmioOut.wait()`. The output wait means
that the result has reached its DDR allocation before the PS reads it. Its
in-place example separately waits for input to be copied into array-local
memory before admitting a write to that external range. These guarantees
belong to the documented GMIO mapping and channel owner.
[GMIO programming model][gmio-model]

The corresponding XRT edge implementation owns an array through ZYNQ/ZOCL.
Ordinary GMIO configuration sets the AXI cache operand to zero.
`gmio_api::wait` waits for channel completion, recycles completed BD IDs,
synchronizes the retained BOs and then releases those references. The outer
blocking operation also synchronizes its buffer. Its separate two-buffer
external ping-pong path returns immediately from the external-buffer wait;
that call is not retirement of the continuing graph. These concrete
operations explain the managed flow without transferring its non-cacheable
mapping or completion contract to a Ryzen shared-memory flag protocol.
[XRT array owner][gmio-owner] [GMIO configuration][gmio-config]
[GMIO completion owner][gmio-wait] [Blocking BO operation][gmio-sync]
[External ping-pong wait][gmio-pingpong]

[bo-kinds]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h#L216-L223
[create-uapi]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h#L225-L264
[share-create]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1045-L1072
[heap-create]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1075-L1129
[dev-create]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1137-L1174
[heap-view]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/buffer.cpp#L505-L519
[shmem-create]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L877-L889
[shmem-mmap]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/drm_gem_shmem_helper.c#L752-L789
[bo-info]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1369-L1399
[address-selection]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L274-L280
[address-mode]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.h#L91-L118
[user-create]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L892-L928
[user-caller]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/host/platform_host.cpp#L156-L203
[user-ranges]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_ubuf.c#L246-L365
[kmq-policy]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/kmq/pcidev.cpp#L43-L48
[bo-sync]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/common/api/xrt_bo.cpp#L506-L524
[line-size]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/buffer.cpp#L33-L37
[direct-flush]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/buffer.cpp#L129-L164
[sync-selection]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/buffer.cpp#L166-L171
[shim-sync]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/buffer.cpp#L648-L681
[initial-flush]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/buffer.cpp#L443-L472
[sync-uapi]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h#L286-L301
[flush-helper]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1402-L1433
[driver-sync]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L1436-L1508
[drm-range]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/drm_cache.c#L148-L177
[drm-pages]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/drm_cache.c#L50-L145
[tensor-init]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/xrtruntime/tensor.py#L75-L141
[tensor-sync]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/xrtruntime/tensor.py#L33-L62
[tensor-reconcile]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/tensor_class.py#L413-L465
[tensor-read]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/tensor_class.py#L677-L690
[host-run]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/xrtruntime/hostruntime.py#L250-L320
[command-slot]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_message.c#L716-L746
[command-publish]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_message.c#L1158-L1201
[shim-submit]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/host/platform_host.cpp#L274-L295
[exec-ioctl]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_ctx.c#L742-L763
[argument-owner]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_ctx.c#L569-L623
[saxpy]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/programming_examples/getting_started/01_SAXPY/saxpy.py#L42-L95
[saxpy-host]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/programming_examples/getting_started/01_SAXPY/saxpy.py#L98-L117
[task-resolve]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/runtime/dmatask.py#L104-L136
[task-group]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/runtime/runtime.py#L89-L134
[responses]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L340-L441
[notify]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L277-L294
[import-owner]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L998-L1033
[import-release]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L656-L662
[user-pin]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_ubuf.c#L229-L241
[hmm-owner]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_gem.c#L283-L333
[worker-default]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/worker.py#L42-L60
[worker-loop]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/worker.py#L240-L267
[gmio-model]: https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/Programming-Model-for-AI-Engine-ML-to-DDR-Memory-Connection
[gmio-owner]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/aie.cpp#L34-L87
[gmio-config]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/common_layer/adf_runtime_api.cpp#L1000-L1029
[gmio-wait]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/common_layer/adf_runtime_api.cpp#L1115-L1138
[gmio-sync]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/aie.cpp#L295-L329
[gmio-pingpong]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/aie.cpp#L281-L293
