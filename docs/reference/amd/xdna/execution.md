# Native XDNA execution

A native XDNA command asks firmware to execute a controller program in a
workload context. That program configures the array, starts tile workers and
DMA, and joins the work whose completion it reports. Host submission, firmware
response, successful execution and the last use of each memory resource are
distinct events.

## Architecture and transport

The AMD native device table distinguishes these client products:

| Native product | PCI device / revision |
| --- | --- |
| NPU4 | `0x17f0 / 0x10` |
| NPU5 | `0x17f0 / 0x11` |
| NPU6 | `0x17f0 / 0x20` |

These entries select device-specific operations and firmware data; they do not
encode array dimensions. NPU6 uses the AIE2 driver path and shares NPU4
firmware configuration, hardware operations and feature tables in the cited
driver. That relationship does not establish compatibility of arbitrary
executable images or equal context geometry. [Device table][device-table]
[NPU6 configuration][npu6]

The Linux AIE2 path described below uses `amdxdna` DRM contexts and firmware
mailboxes. Management operations use the privileged firmware context; each
workload has a separate user channel. Firmware controls partition setup and
enforces the resource solver's context-to-column binding. Spatial partitions
carry the active context's PASID when accessing host memory. [Native
architecture][native-architecture]

Windows uses the MCDM execution model described in Microsoft's architecture
document (updated April 24, 2025). Its driver turns work into native DMA
buffers assigned to a context and address space; the OS scheduler assigns
engine time and handles preemption. The engine reports completion in
submission order. These are scheduling and ownership requirements, not a
definition of AMD's private context-allocation or transaction-buffer layout.
The Linux mailbox representation must not be assumed to be the Windows
transport. [MCDM architecture][mcdm]

## Linux context and command representation

`DRM_IOCTL_AMDXDNA_CREATE_HWCTX` supplies workload resource and QoS inputs and
returns a native context handle and synchronization object. The AIE2 driver
sends firmware the selected starting column, column count, unused-column
count, PASID and context priority. Firmware returns the context identity and
mailbox resources. The driver then creates the context's mailbox channel.
[Context UAPI][context-uapi] [Firmware context creation][context-create]

The ordinary execution UAPI separates the command BO from the storage reached
by the controller program:

| Field | Representation and meaning |
| --- | --- |
| `hwctx` | 32-bit native context handle. |
| `type` | 32-bit command type; ordinary execution uses `AMDXDNA_CMD_SUBMIT_EXEC_BUF`. |
| `cmd_handles` | 64-bit field containing one BO handle directly when `cmd_count` is one, otherwise representing an array under the UAPI definition. |
| `cmd_count` | 32-bit command-handle count. The cited ordinary ioctl implementation accepts one command BO. |
| `seq` | Returned 64-bit sequence number identifying accepted work. |
| `ext`, `ext_flags` | Must be zero in this UAPI revision. |

The UAPI also defines argument-array fields. The cited ordinary ioctl calls
the job constructor with no argument BO list; those fields do not turn the
controller's embedded addresses into an automatically retained dependency
graph. [Execution UAPI][execution-uapi] [Ordinary ioctl caller][exec-ioctl]

For `ERT_START_NPU`, the command payload starts with a 64-bit
instruction-buffer address, a 32-bit instruction size in **bytes**, and a
32-bit property count. Properties and ordinary arguments follow. This is
different from `ERT_START_DPU`, whose representation also carries trace-buffer
and controller fields, and from the preemption forms with explicit
save/restore payloads. The command opcode selects the representation. [Command
payloads][command-layout] The command-list builder accepts the preemption
forms only when the selected device has the `AIE2_PREEMPT` feature. Their
payload definitions alone do not establish device admission. [Opcode
admission][command-admission]

The AIE2 runtime selects single-command or command-list mailbox construction.
`force_cmdlist` defaults to true at the cited revision, so a single command BO
can still be copied into a driver-owned command-list slot. The DPU slot
builder copies the instruction address, byte size, properties and arguments;
it does not copy the indirectly addressed instruction stream. The ordinary job
runner uses a separate response handler for the list form. [Command-list
policy][command-list-policy] [Selection and dispatch][job-run] [Slot
construction][dpu-slot] [Single-command envelope][single-envelope]

Native transport storage and executable instruction storage consequently have
different owners. A command count in the submission envelope is not a tile
workgroup count, and a copied envelope does not permit the caller to overwrite
the instruction bytes that firmware will fetch.

## Acceptance, completion and result inspection

Submission obtains a native job credit, constructs scheduler/fence state,
assigns a sequence number, pushes the job and attaches its completion point to
the context's synchronization object. Credit acquisition can wait. A returned
sequence establishes accepted work, not worker readiness or a completed DMA.
[Job publication][job-submit]

The job runner sends a mailbox request with a job handle and response
callback. A successful send is separate from the later firmware response. For
a single command, the handler records `COMPLETED`, `ERROR`, `ABORT` or timeout
state. For a command list, it also interprets the failed-command index and
status. Both handlers reach the notification path, which signals the job fence
and returns the native job credit. Fence readiness therefore accompanies
terminal errors as well as success; command-result inspection is part of
normal completion. [Response handlers][responses] [Fence notification][notify]

The native wait UAPI names a context and sequence; its timeout is in
milliseconds and zero means an infinite host wait. This does not change the
device's execution budget. The AIE2 driver separately owns timeout detection
and recovery, with `tdr_timeout_ms` and scheduler-version-dependent progress
logic. A host timeout does not itself cancel accepted work or establish that
its backing is no longer reachable. [Wait UAPI][wait-uapi] [Timeout
policy][timeout-policy]

## Placement and program quiescence

Native scheduling transfers permission to execute on a placement. It is not an
application-managed exclusion lock between unrelated processes. A context can
be time-shared with another context, so a retained context handle is not a
reservation of application registers, locks, routes or tile-local data between
independent commands. Firmware's placement binding and an application's
program state answer different questions. [Spatial and temporal
scheduling][native-architecture]

Each independent entry establishes the state it consumes. This does not mean
every register is zero at entry: a correct program initializes required state
without assuming either zero or a predecessor's values. Nor should it apply
fresh-entry initialization to an invocation being resumed under an established
native preemption protocol.

The controller program owns its workers and transfers. Before its terminal
operation, it satisfies admitted reads, stops further production, joins tile
workers and drains DMA which could interfere with subsequent configuration or
write released storage. A worker blocked on a stream needs that stream's wake
protocol; changing a separate stop word does not make the read complete. The
firmware response is an observation of the submitted controller protocol, not
an automatic join of arbitrary autonomous work it started.

The distinction is especially important for program replacement. A replacement
inside one still-running invocation can use its own transfer,
instruction-fetch exclusion and state-handoff protocol. A later independent
native command establishes a new entry state. Completion of an NPU invocation
also does not join a separately scheduled GPU consumer of its output. Such a
consumer has its own completion and memory-visibility edge.

## A complete finite programming sequence

1. Create the native context for the intended placement and obtain the address
   interpretations required by its instruction and DMA users. Allocate command,
   executable, payload and result storage under those native mappings.
2. Prepare a complete establishing controller program: array configuration,
   tile code, routes, locks and DMA descriptors. Publish its immutable bytes and
   CPU-produced inputs with the backing's native memory-visibility operations.
3. Submit the command envelope. Keep executable bytes, embedded arguments and
   every reachable allocation valid while any controller, tile or DMA user can
   access them. Acceptance can precede actual execution.
4. The program starts its workers and performs the intended dataflow. Its final
   controller sequence joins the workers and relevant DMA, including result,
   counter-reply or trace transfers.
5. Observe the native completion point and inspect command status. Apply the
   destination's visibility operation before CPU readback or publish a separate
   dependency to a device consumer.
6. Reuse or release each range only after its final user. Retiring the command
   envelope, observing controller completion and joining downstream consumers
   can release different storage at different times.

The Linux job owner retains explicitly acquired command/argument BO references
and releases them in job cleanup. It cannot infer arbitrary addresses embedded
inside opaque controller instructions. A live mapping supplies address access;
it is not an execution dependency or a complete last-use certificate. [Job
construction and cleanup][job-owners]

For observation workloads, source stop, source flush and destination DMA
completion are separate actions. The [observability chapter](observability.md)
describes those joins, timer epochs and result formats.

## Runtime power and residency

The AIE2 suspend helper waits up to two seconds for the final recorded job
fence, then stops contexts before stopping hardware. It does not check that
wait's result, so suspension is not a workload's successful-completion
certificate. Resume restarts hardware and recreates contexts. Context restart
remaps the retained heap and configures compute units; it is not an
application tile-state checkpoint. Host objects can therefore survive a native
suspend/resume cycle without preserving application state in the array.
[Context suspend and restart][context-resume] [Device suspend and
resume][device-resume]

Linux runtime-PM `power/control=on` prevents runtime autosuspend; `auto`
permits it. The device-wide policy owner records the prior setting, verifies
the active state required by its workload and restores that setting when its
policy period ends. Closing a process or context is not restoration of a sysfs
policy write. This control neither fixes the operating frequency nor reserves
a placement or preserves tile state. Windows needs its own native power
contract; the Linux attribute is not a portable control. [Runtime-PM
interface][runtime-pm]

Host timing around native submission can include synchronous hardware and
firmware resume before work is accepted. Holding runtime power active and
measuring a deployment's normal idle/wake behavior describe different
intervals. A warm command or a retained context alone does not establish a
continuous power hold. Device-clock conversion additionally needs the sampled
clock domain, frequency history and reset epoch.

[device-table]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_pci_drv.c#L79-L84
[npu6]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/npu6_regs.c#L65-L124
[native-architecture]: https://www.kernel.org/doc/html/latest/accel/amdxdna/amdnpu.html
[mcdm]: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mcdm-architecture
[context-uapi]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h#L77-L104
[context-create]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_message.c#L313-L393
[execution-uapi]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h#L302-L332
[exec-ioctl]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_ctx.c#L741-L763
[command-layout]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_ctx.h#L20-L91
[command-admission]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_message.c#L950-L987
[command-list-policy]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L27-L29
[job-run]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L444-L505
[dpu-slot]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_message.c#L716-L746
[single-envelope]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_message.c#L1038-L1071
[job-submit]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L1249-L1329
[responses]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L340-L441
[notify]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L277-L295
[wait-uapi]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h#L334-L350
[timeout-policy]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L31-L76
[job-owners]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_ctx.c#L570-L736
[context-resume]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c#L148-L274
[device-resume]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_pci.c#L466-L497
[runtime-pm]: https://docs.kernel.org/power/runtime_pm.html
