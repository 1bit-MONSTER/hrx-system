# Tile DMA and task ownership

AI Engine DMA moves data between memory and the array's streams. An MM2S
channel reads memory and produces a stream; an S2MM channel consumes a stream
and writes memory. Buffer descriptors (BDs) describe the memory accesses and
local lock operations. A channel's task queue selects a descriptor chain and
its repetition count. Task completion, descriptor availability and the last
use of transferred data are separate events.

## Architecture and address spaces

This chapter describes the AIE2P property tables in the AMD AI Engine driver
and the MLIR-AIE `NPU2` target, which selects the `AIE2p` architecture. The
Versal AIE-ML v2 Architecture Manual, AM027 revision 1.1 (October 23, 2025),
provides the corresponding tile and array-interface descriptions. Its Versal
NoC and programmable-logic integration does not establish the Ryzen NPU's
external-memory coherence or address translation. Those belong to the native
context and mapping described in [native execution](execution.md) and the
[GPU/NPU handoff](../gpu/recipes/gpu-npu.md). [NPU2 target][npu2]
[AM027 array interface][am027-interface]

| AIE2P DMA location | Channels in each direction | BD slots | Local locks | Memory reached by the DMA |
| --- | --- | --- | --- | --- |
| Compute-tile memory module | 2 | 16 | 16 | The tile's 64 KiB data memory. |
| Memory tile | 6 | 48 | 64 | Local 512 KiB; channels 0–3 can also reach the adjacent west and east memory tiles. |
| Shim NoC tile | 2 | 16 | 16 | External memory through a 48-bit AXI address and the native mapping. |

The driver tables supply the channel and descriptor counts. AM027 supplies
the physical memories and neighbor-access rules. The shim's address is an
external DMA address, not an arbitrary CPU virtual pointer or GPU virtual
address. [Compute DMA resources][core-module] [Memory-tile DMA
resources][memory-module] [Shim DMA resources][shim-module]
[AM027 compute memory][am027-core] [AM027 memory-tile memory][am027-memory]
[AM027 memory-tile DMA][am027-memory-dma]

The memory-tile DMA's address and lock operands use a three-tile window:

| Selected memory | Byte address range | DMA lock IDs | Channels |
| --- | --- | --- | --- |
| West neighbor | `0x000000`–`0x07ffff` | 0–63 | 0–3 |
| Local tile | `0x080000`–`0x0fffff` | 64–127 | 0–5 |
| East neighbor | `0x100000`–`0x17ffff` | 128–191 | 0–3 |

Thus the driver's 192 reachable lock IDs describe three sets of 64 physical
locks. They do not provide 192 local semaphore objects. Memory-tile channels
also have a BD-bank restriction: even-numbered channels use BDs 0–23 and
odd-numbered channels use BDs 24–47, in either direction. Placement, channel,
buffer address and lock ID must describe the same reachable owner.
[AM027 memory-tile DMA][am027-memory-dma] [AM027 locks][am027-locks]
[BD/channel selection][bd-channel]

## Descriptor registers and units

A BD occupies a 32-byte register slot. The ordinary MLIR-AIE emitter writes
six DWORDs for a compute-tile BD and eight for a memory-tile or shim BD; the
compute payload size does not reduce the slot stride. In the NPU2 target
model, the tile-relative BD bases are `0x1d000` for compute and shim tiles and
`0xa0000` for memory tiles. The register address combines the column shifted
by 25, row shifted by 20, and tile-relative offset. These are register-space
addresses, distinct from the payload address inside a BD. [BD register
addressing][bd-address] [Target address units][target-units]
[BD word emission][bd-word-count]

The driver descriptor API accepts a byte address and byte length. AIE2P
addresses have four-byte alignment. Length is encoded in DWORDs with no
length-minus-one adjustment. Local-memory addresses are encoded in DWORDs;
the shim retains a byte address split across its low and high fields. This
granularity concerns the DMA representation: a tensor of 16-bit elements can
still be transferred when its selected access pattern forms valid DWORD
accesses. [Address and length conversion][address-length]
[Compute address properties][core-address-properties]
[Memory address properties][memory-address-properties]
[Shim address properties][shim-address-properties]

The principal address, length and packet fields are:

| Operand | Compute BD | Memory-tile BD | Shim BD |
| --- | --- | --- | --- |
| Buffer length, DWORDs | word 0 `[13:0]` | word 0 `[16:0]` | word 0 `[31:0]` |
| Buffer address | word 0 `[27:14]`, DWORD address | word 1 `[18:0]`, DWORD address in the three-tile window | word 1 `[31:0]` byte-address low; word 2 `[15:0]` high |
| Packet enable | word 1 `[30]` | word 0 `[31]` | word 2 `[30]` |
| Out-of-order BD ID | word 1 `[29:24]` | word 0 `[22:17]` | word 2 `[29:24]` |
| Packet ID | word 1 `[23:19]` | word 0 `[27:23]` | word 2 `[23:19]` |
| Packet type | word 1 `[18:16]` | word 0 `[30:28]` | word 2 `[18:16]` |

The ordinary emitter initializes its word array to zero and fills these
fields. Shim address patching subsequently supplies the native buffer
address; local-buffer patching converts the assigned address to DWORDs and
adds the AIE2P memory-tile window base. A zero-filled template is therefore
not the complete externally addressed transfer. [Compute BD fields][core-bd]
[Memory BD fields][memory-bd] [Shim BD fields][shim-bd]
[Native shim address split][shim-address-split]
[Native address patching][address-patch]

### Multidimensional access and iteration

The ordinary address generator has three dimensions on compute and shim
tiles, and four on memory tiles. A separate iteration offset supplies a
further dimension. Steps are in DWORDs and are encoded as step minus one.
The ordinary dimension wrap fields carry their wrap counts directly;
iteration wrap is encoded as count minus one. Iteration current is a
separate six-bit state field. These encodings must not be conflated with the
task queue's repeat count. [Dimension conversion][dimension-conversion]
[Native dimension encoding][dimension-encoding] [Iteration setup][iteration-setup]

| Emitted operand | Compute BD | Memory-tile BD | Shim BD |
| --- | --- | --- | --- |
| D0 step | word 2 `[12:0]` | word 2 `[16:0]` | word 3 `[19:0]` |
| D1 step | word 2 `[25:13]` | word 3 `[16:0]` | word 4 `[19:0]` |
| D2 step | word 3 `[12:0]` | word 4 `[16:0]` | word 5 `[19:0]` |
| D0 wrap | word 3 `[20:13]` | word 2 `[26:17]` | word 3 `[29:20]` |
| D1 wrap | word 3 `[28:21]` | word 3 `[26:17]` | word 4 `[29:20]` |
| Iteration step | word 4 `[12:0]` | word 6 `[16:0]` | word 6 `[19:0]` |
| Iteration wrap | word 4 `[18:13]` | word 6 `[22:17]` | word 6 `[25:20]` |
| Iteration current | word 4 `[24:19]` | word 6 `[28:23]` | word 6 `[31:26]` |

These are the fields used by the cited MLIR-AIE word emitter. Its memory-tile
path leaves D2 wrap and D3 step unpopulated; it is not evidence that the
hardware lacks the fourth address dimension. Its static task lowering uses
the transfer length to bound D2 and treats a contiguous shim access as a
linear transfer, rather than forcing it through the ten-bit wrap fields.
[Compute BD fields][core-bd] [Memory BD fields][memory-bd]
[Shim BD fields][shim-bd] [Static dimensional lowering][dimensions]

Memory-tile MM2S constant padding inserts a configured 32-bit value into the
stream. Before/after padding belongs to the lowest three dimensions and is
additional to their wraps. The direct emitter places before counts in word 1
`[31:26]`, word 3 `[31:27]`, word 4 `[30:27]`, and after counts in word 5
`[22:17]`, `[27:23]`, `[31:28]`, respectively. Padding is not an external
memory access for each inserted value. [AM027 memory-tile DMA][am027-memory-dma]
[Memory BD fields][memory-bd]

The shim burst field is word 4 `[31:30]`. On AIE2P, values 0–3 select 4, 8,
16 or 32 beats of the 128-bit AXI data interface: 64, 128, 256 or 512 bytes.
The native driver API takes beats; MLIR-AIE's burst-length input takes bytes.
`AxCACHE` occupies word 5 `[27:24]`; the target-model default is 2. That AXI
attribute neither supplies a CPU cache-maintenance operation nor establishes
an NPU/GPU coherent mapping. [AIE2P burst conversion][burst-driver]
[NPU2 burst lengths][npu2] [Shim AXI setup][axi-setup]
[AxCACHE default][axcache]

## Local locks and descriptor chains

AIE2-family locks are counting semaphores, unlike AIE1's binary locks.
AM027 describes six-bit unsigned stored state. The DMA acquire and release
operands are seven-bit quantities with signed-operation meaning. MLIR-AIE
lowers `AcquireGreaterEqual(n)` by encoding `-n`: execution waits for at least
`n` and consumes `n` units. Release adds the specified amount. The stored
counter width and operand width describe different quantities.
[Lock semantics][lock-semantics] [Lock encoding][lock-encoding]
[AM027 locks][am027-locks]

| Control operand | Compute word 5 / shim word 7 | Memory-tile BD |
| --- | --- | --- |
| Valid BD | bit 25 | word 7 bit 31 |
| Acquire enable | bit 12 | word 7 bit 15 |
| Acquire lock ID | bits `[3:0]` | word 7 `[7:0]` |
| Acquire value | bits `[11:5]` | word 7 `[14:8]` |
| Release lock ID | bits `[16:13]` | word 7 `[23:16]` |
| Release value | bits `[24:18]` | word 7 `[30:24]` |
| Use next BD | bit 26 | word 1 bit 19 |
| Next BD ID | bits `[30:27]` | word 1 `[25:20]` |

There is no separate release-enable bit in these emitted layouts. The
software descriptor's `LockRelEn` member must not be read as another hardware
bit: the native writer packs release ID and value. AM025 revision 1.2
(January 29, 2026) explicitly defines zero as no release action for the
matching AIE-ML shim lock word. MLIR-AIE's ordinary object-FIFO DMA lowering
emits acquire, transfer, release, then the next-BD edge. The locks transfer
ownership of the associated local buffer between DMA and tile participants.
[Native lock descriptor][driver-set-lock] [Native lock word][driver-lock-word]
[AM025 shim lock word][am025-lock-word] [Object-FIFO DMA sequence][fifo-dma]

`Use_Next_BD` connects register-resident descriptors. A finite traversal ends
at a BD without that edge; a cyclic local dataflow can keep revisiting its buffer
pool under locks. Every reachable BD and lock remains part of that channel's
program while a queued or active task can use it, including repeated visits.
Releasing one buffer does not terminate a cyclic chain. The compiler's native
configuration path installs next-BD links, marks BDs valid, writes them, then
queues and enables the channel. [BD installation][bd-install]
[Channel start][channel-start]

## Task queues and completion tokens

The AIE2P tables specify four task-queue entries per channel and an execution
count of 1–256. The start-queue register is the channel-control register plus
four bytes. The NPU2 target uses these tile-relative control bases, with
eight bytes between channels:

| Tile | S2MM channel 0 | MM2S channel 0 |
| --- | --- | --- |
| Compute | `0x1de00` | `0x1de10` |
| Memory | `0xa0600` | `0xa0630` |
| Shim | `0x1d200` | `0x1d210` |

[Compute task capacity][core-task-capacity] [Memory task
capacity][memory-task-capacity] [Shim task capacity][task-capacity]

The pushed DWORD contains the head BD ID in `[3:0]` for compute/shim or
`[5:0]` for memory tiles, execution-count-minus-one in `[23:16]`, and token
enable in bit 31. The driver's `RepeatCount` argument is the actual count;
the MLIR task attribute already holds count minus one. Thus both paths encode
zero for a single traversal. For token-producing tasks with an assigned
controller ID, the direct emitter also programs channel-control `[12:8]`.
[Task capacity][task-capacity] [Channel addresses][channel-address]
[Native task publication][task-start] [Queue word emission][queue-word]

A task-complete token (TCT) identifies completion of the selected task. The
controller must enable token production and consume the corresponding token;
MLIR-AIE rejects an await on a task that does not issue one. A token for one
channel does not implicitly join another independent channel.
[AM027 task completion][am027-memory-dma] [Await lowering][task-await]

Queue capacity and completed execution have different observations. The
native `WaitForBdTaskQueue` polls the task-queue-size field to find a free
entry. `WaitForDone` additionally checks that the channel is not running and
that lock, stream and TCT stalls are absent. A free queue entry can coexist
with an active descriptor. Neither observation alone transfers a result from
an NPU mapping into a different CPU/GPU cache domain.
[Capacity wait][capacity-wait] [Channel completion wait][done-wait]

AIE2IPU and AIE2P S2MM also have a channel-level **finish-on-TLAST** mode.
Here the programmed buffer length is capacity, and a packet's `TLAST` can
complete a shorter transfer. The selected mode reports no count, sends the
actual word count with a task token, or exposes it through a count FIFO. The
[completion-mode discussion](observability.md#stop-flush-and-dma-completion)
describes the controls; [completed-count consumption](observability.md#consuming-completed-transfer-counts)
describes the single-reader FIFO-pop ownership and backpressure. A packet end
does not by itself stop a multi-packet stream or retire its routes.

## External completion in the managed GMIO flow

AMD's AI Engine-ML Kernel and Graph Programming Guide, UG1603 revision
2026.1 (July 8, 2026), gives output completion a concrete external-memory
meaning. After `aie2gm_nb` submits output, `gmioOut.wait()` waits for that
output to reach the allocated DDR range before the processing system (PS)
reads it. The documented Linux flow uses `GMIO::malloc` and non-cacheable
memory. This is a delivery guarantee for that managed mapping and observer,
not merely acceptance of the output stream. [GMIO programming model][gmio-model]

The cited XRT edge implementation constructs its array through ZYNQ/ZOCL.
Its ordinary GMIO setup programs `Cache=0`, whereas the MLIR-AIE shim policy
described above uses `AxCACHE=0x2`. `gmio_api::wait` calls
`XAie_DmaWaitForDone`, recycles the completed BD IDs and synchronizes retained
buffer objects before dropping those references. The outer blocking
`sync_bo` path also synchronizes its buffer after the wait. Thus the native
channel join and the buffer's host-access operation remain explicit parts
of the API flow. [XRT array owner][gmio-owner]
[GMIO configuration][gmio-config] [GMIO wait][gmio-wait]
[Blocking buffer synchronization][gmio-sync]

UG1603 also describes an intermediate external DDR/LPDDR ping-pong buffer
between two kernels. The caller supplies both buffers, starts the graph and
waits for the final output; no intermediate host join transfers each buffer.
In XRT's external-buffer path, compiler-supplied port metadata identifies
the BDs, buffer indices and offsets, task repetition and completion-token
policy. The runtime derives addresses from the supplied buffers, patches the
BDs and queues the tasks. Its two-buffer mode returns
immediately from the external-buffer wait and status methods: those methods
do not establish retirement of the continuing ping-pong graph.
[Managed external buffers][gmio-external] [External-buffer owner][external-owner]

These flows establish ordinary external delivery and managed external
dataflow on their documented platform. They do not specify the generated
ping-pong graph's local lock schedule, the earlier release point of one BD
in a continuing channel, or the Ryzen GPU's memory-observation point. A
resident ready-flag protocol composes those separate properties as described
in the [GPU/NPU handoff](../gpu/recipes/gpu-npu.md#output-dma-and-a-ready-flag).

## One complete IRON transfer flow

The IRON SAXPY example moves two external tensors into a worker and returns
one tensor. Its concrete input is 4096 `bfloat16` elements per tensor, or
8192 bytes. The worker acquires X, Y and a free Z object, computes `3*X+Y`,
then releases the three objects. The runtime issues two input `fill`
operations and one output `drain(wait=True)`. When compiled for NPU2, this
provides a finite shim-DMA owner flow using the AIE2P target above.
[SAXPY caller][saxpy]

1. **Establish the array and external owners.** Placement assigns tiles,
   channels, local buffers, locks and stream endpoints. The native context
   supplies the address interpretation for X, Y and Z. The caller retains
   their external allocations and publishes CPU-written input through that
   mapping's host/device synchronization. Local object-FIFO DMA descriptors
   acquire the appropriate pool lock, transfer an object and release the next
   participant's lock. [Host input synchronization][tensor-transfer]
   [XRT transport synchronization][xrt-sync]
   [Object-FIFO DMA sequence][fifo-dma]
2. **Construct the three runtime tasks.** Each task names its shim allocation
   metadata and external buffer argument. `DMATask.resolve` passes `wait` as
   `issue_token`, builds a single-BD task and starts it. The Python helper
   expresses offsets and tensor dimensions in elements, normalizes them to
   four dimensions, and makes the outer dimension the repeat count. The lower
   three dimensions determine the length of one BD traversal. For this
   linear example, one traversal transfers 2048 DWORDs.
   [Task resolution][task-resolve] [Single-BD helper][single-bd]
3. **Publish complete native descriptors before their queue entries.** The
   compiler assigns BD IDs, converts element quantities to bytes and DWORDs,
   emits the BD register block write, and patches the external address from
   the runtime argument plus its byte offset. The task-start operation then
   writes the head BD, repeat and token bits into the channel's task queue.
   The output task requests a TCT. [BD conversion][bd-conversion]
   [Native address patching][address-patch] [Task-start lowering][task-lowering]
4. **Transfer ownership through the dataflow.** The input DMAs supply complete
   local objects before the worker acquires them. The worker's Z release
   makes its produced object available to the output DMA. In this particular
   finite graph, finishing the output requires the worker to have consumed
   both inputs. This is the dependency that permits the unawaited input
   tasks' resources to be recycled after the output wait; a task group of
   unrelated transfers would not have that property. [SAXPY caller][saxpy]
   [Dependent-transfer reuse contract][dependent-reuse]
5. **Join output before recycling.** Runtime finalization emits the group's
   waits before its frees. The output await becomes `NpuSync` for the selected
   tile, direction and channel, then a firmware TCT transaction. The static
   allocator returns BD IDs at await/free points and erases explicit free
   operations. A free itself is compiler bookkeeping, not a device wait.
   [Group finalization][task-group] [Await lowering][task-await]
   [Static recycling][static-recycle] [Free erasure][free-erasure]
6. **Observe native success and acquire the result.** The ordinary
   instruction-buffer XRT path retains the instruction BO and argument BOs
   while it calls the kernel and waits for its run handle. It checks for
   `ERT_CMD_STATE_COMPLETED` before returning ordinary success. The caller
   then obtains the output through the host runtime's tensor readback, with
   that mapping's cache synchronization. A terminal command error is not a
   successful output transfer. [XRT invocation and wait][xrt-wait]
   [SAXPY readback][saxpy-readback] [Tensor readback][tensor-readback]
   [XRT transport synchronization][xrt-sync]

At the controller-command layer, a BD write is a firmware `BLOCKWRITE`
transaction (opcode 1); task publication is `WRITE` (opcode 0). The await
becomes `TCT` (opcode `0x80`), four DWORDs long: word 1 is its 16-byte operation
size, word 2 contains direction `[7:0]`, row `[15:8]` and column `[23:16]`,
and word 3 contains row count `[15:8]`, column count `[23:16]` and channel
`[31:24]`. This task await uses a one-row, one-column region. The compiler
explicitly distinguishes this firmware transaction numbering from an older
driver transaction enum. [Transaction opcodes][txn-opcodes]
[Transaction encoding][txn-encoding] [Controller translation][txn-lowering]

The example's default worker is repeated: `Worker` wraps its body in a
`range_(sys.maxsize)` loop unless configured otherwise. Returning this finite
output therefore does not stop the worker or release the local cyclic DMA,
lock and route program. Replacing that resident program requires the separate
worker/channel quiescence protocol described in
[native execution](execution.md#placement-and-program-quiescence).
[Worker repetition][worker-default] [Worker body lowering][worker-loop]

## Final use and architecture boundaries

| Resource | Boundary permitting reuse |
| --- | --- |
| Host-side descriptor object | Its native register-write operation has consumed the software representation; the installed BD still has its own lifetime. |
| Hardware BD slot and chain links | No queued, active or repeated task can reach them; compiler allocation alone does not establish this. |
| Tile-local FIFO object | The relevant DMA/core consumer releases ownership through the local lock protocol. |
| External input | Its last DMA read completes, directly or through a complete dependent output path such as the finite SAXPY graph. |
| External output | Its producing transfer completes; each later CPU/GPU consumer applies its own visibility and final-use protocol. |
| Controller instructions and native arguments | The native command has successfully completed all relevant uses; indirect data owners remain live through their actual users. |
| Resident local buffers, locks and routes | All workers and channels that can revisit them are quiescent. |

These lifetimes follow the installed chain, task completion and actual caller
flow above. They are not interchangeable with a software `dma_free_task` or
the availability of another task-queue entry.

The source-selected AIE2P details also limit what can be carried to another
target. AIE1 has binary lock semantics and lacks the driver's task-repeat/TCT
feature. The NPU2 target's 512-byte burst choice is architecture-specific.
The separately named `AIE2PS` target even changes the shim BD base to `0x9000`
and stride to `0x30`; the AIE2P register table is not a universal “AI Engine”
layout. Driver `AddrMax` values are accepted address apertures, not physical
SRAM capacities. [Native task applicability][task-applicability]
[Lock semantics][lock-semantics] [AIE2PS register differences][aie2ps]
[Compute address properties][core-address-properties]

Finally, a local lock released by an S2MM payload transfer is not, by itself,
an established publication of that payload to a GPU which observes a flag
written by another channel. The missing edge is the lock release relative to
all relevant external write responses, together with the system's common
memory-observer contract. The ordinary finite task and host-wait flow above
does not supply that stronger cross-channel guarantee. The
[GPU/NPU handoff](../gpu/recipes/gpu-npu.md) treats those mapping, ordering and
observer requirements separately.

[npu2]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIE/IR/AIETargetModel.cpp#L1609-L1615
[gmio-model]: https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/Programming-Model-for-AI-Engine-ML-to-DDR-Memory-Connection
[gmio-external]: https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/AI-Engine-ML-External-Memory-Access
[gmio-owner]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/aie.cpp#L34-L87
[gmio-config]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/common_layer/adf_runtime_api.cpp#L986-L1029
[gmio-wait]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/common_layer/adf_runtime_api.cpp#L1115-L1138
[gmio-sync]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/aie.cpp#L318-L329
[external-owner]: https://github.com/Xilinx/XRT/blob/ecad6cf22171ffec754fdd36afc2ae200af5c3a6/src/runtime_src/core/edge/user/aie/aie.cpp#L230-L293
[am027-interface]: https://docs.amd.com/r/en-US/am027-versal-aie-ml-v2/AIE-ML-v2-Array-Interface
[am027-core]: https://docs.amd.com/r/en-US/am027-versal-aie-ml-v2/AIE-ML-v2-Memory-Module
[am027-memory]: https://docs.amd.com/r/en-US/am027-versal-aie-ml-v2/AIE-ML-v2-Memory-Tile-Memory
[am027-memory-dma]: https://docs.amd.com/r/en-US/am027-versal-aie-ml-v2/AIE-ML-v2-Memory-Tile-DMA
[am027-locks]: https://docs.amd.com/r/en-US/am027-versal-aie-ml-v2/AIE-ML-v2-Memory-Tile-Locks-Module-and-Stream-Switch
[am025-lock-word]: https://docs.amd.com/r/en-US/am025-versal-aie-ml-register-reference/DMA_BD0_7-NOC_MODULE-Register
[core-module]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L1898-L1937
[memory-module]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L1660-L1699
[shim-module]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L2151-L2190
[bd-channel]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L2254-L2268
[bd-address]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIE/IR/AIETargetModel.cpp#L826-L854
[target-units]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/include/aie/Dialect/AIE/IR/AIETargetModel.h#L708-L858
[bd-word-count]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp#L696-L713
[address-length]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c#L564-L616
[core-address-properties]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L1809-L1832
[memory-address-properties]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L1574-L1597
[shim-address-properties]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L2062-L2085
[core-bd]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp#L815-L861
[memory-bd]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp#L767-L814
[shim-bd]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp#L717-L766
[shim-address-split]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L1449-L1491
[address-patch]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDMATasksToNPU.cpp#L314-L424
[dimension-conversion]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L191-L215
[dimension-encoding]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L1000-L1065
[iteration-setup]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L2465-L2485
[dimensions]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDMATasksToNPU.cpp#L760-L853
[burst-driver]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aie2p.c#L40-L58
[axi-setup]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Targets/AIERT.cpp#L418-L427
[axcache]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/include/aie/Dialect/AIE/IR/AIETargetModel.h#L533-L538
[lock-semantics]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/include/aie/Dialect/AIE/IR/AIEOps.td#L1573-L1592
[lock-encoding]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Targets/AIERT.cpp#L328-L394
[driver-set-lock]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L146-L174
[driver-lock-word]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L1622-L1647
[fifo-dma]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIE/Transforms/AIEObjectFifoLowerDMAs.cpp#L136-L186
[bd-install]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Targets/AIERT.cpp#L568-L599
[channel-start]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Targets/AIERT.cpp#L604-L644
[task-capacity]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L2111-L2147
[core-task-capacity]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L1858-L1895
[memory-task-capacity]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L1620-L1657
[channel-address]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIE/IR/AIETargetModel.cpp#L856-L878
[task-start]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c#L2556-L2621
[queue-word]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp#L159-L226
[task-await]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDMATasksToNPU.cpp#L140-L173
[capacity-wait]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L2180-L2208
[done-wait]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L2113-L2160
[saxpy]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/programming_examples/getting_started/01_SAXPY/saxpy.py#L42-L103
[dependent-reuse]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/include/aie/Dialect/AIEX/IR/AIEX.td#L604-L621
[task-resolve]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/runtime/dmatask.py#L104-L136
[single-bd]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/dialects/aiex.py#L319-L427
[bd-conversion]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDMATasksToNPU.cpp#L646-L709
[task-lowering]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEDMATasksToNPU.cpp#L52-L88
[task-group]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/runtime/runtime.py#L89-L155
[static-recycle]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEAssignRuntimeSequenceBDIDs.cpp#L215-L251
[free-erasure]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIEX/Transforms/AIEAssignRuntimeSequenceBDIDs.cpp#L295-L318
[xrt-wait]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/xrtruntime/hostruntime.py#L250-L320
[tensor-transfer]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/tensor_class.py#L413-L465
[tensor-readback]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/tensor_class.py#L677-L691
[xrt-sync]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/utils/hostruntime/xrtruntime/tensor.py#L54-L62
[saxpy-readback]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/programming_examples/getting_started/01_SAXPY/saxpy.py#L98-L117
[txn-opcodes]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/include/aie/Runtime/TxnEncoding.h#L25-L62
[txn-encoding]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/include/aie/Runtime/TxnEncoding.h#L139-L200
[txn-lowering]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Targets/AIETargetNPU.cpp#L62-L94
[worker-default]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/worker.py#L42-L60
[worker-loop]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/python/iron/worker.py#L240-L267
[task-applicability]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c#L2460-L2483
[aie2ps]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIE/IR/AIETargetModel.cpp#L1623-L1645
