# XDNA timing, counters, and trace

XDNA has several observation mechanisms, each measuring a different part of
execution:

| Mechanism | Observation | Result transport |
| --- | --- | --- |
| Core cycle counter | Time between instructions in a tile program. | The program stores counter samples with its other output. |
| Event counters | Selected core, memory, stream, or DMA activity. | Array control-packet replies or firmware register-read results. |
| Event trace | Changes in selected events, with cycle timing. | Packet streams routed through the array to a DMA destination. |
| Firmware timer records | Markers encountered by the native instruction interpreter. | A firmware result buffer associated with the context. |
| Firmware diagnostic trace | Driver/firmware activity outside the tile program. | A separate device-wide diagnostic channel. |

Tile programs, array control packets and native firmware interfaces expose
these mechanisms through different resource owners. A sample's architecture,
module, clock epoch, transport and result-completion edge determine what it
measures. Register definitions alone do not establish that an operating-system
interface permits a host to access those registers directly.

## Timing a tile program

An AIE2P program reads the tile's 64-bit cycle counter with `mov` from `cntr`
into a register pair. The C++ intrinsic is `get_cycles()`. Samples around a
code region measure elapsed tile-clock cycles, including stalls encountered
between the samples. They do not include host submission or completion
notification. [Cycle-counter intrinsic][cycle-counter]

```asm
mov r9:r8, cntr
// Instructions being measured.
mov r13:r12, cntr
```

The program can store those samples alongside its normal output. Reading the
counter does not require a host call, an OS profiling session, or a trace DMA
channel. The compiler controls instruction placement around each sample; the
measurement includes any instrumentation instructions between them.

For a constant tile frequency `frequency_hz`, elapsed seconds are `(end_ticks
- begin_ticks) / frequency_hz`. Saving only the low 32 bits shortens the
unambiguous interval: at 1.8 GHz it wraps in about 2.386 seconds. A pair of
64-bit samples avoids that short wrap interval, but still belongs to one timer
epoch. Timer reset, placement reuse, and clock gating are distinct from
counter wrap. The [native execution
lifetime](execution.md#placement-and-program-quiescence) does not preserve
application timer state between independent submissions.

Host time around native submission includes admission and any synchronous
resume of an idle device. Extending that interval through successful native
completion and result inspection includes remaining scheduling, execution and
completion observation. Output acquisition is a separate step unless the
measured interval explicitly includes it. Neither interval is interchangeable
with a pair of in-program counter reads. For a resident worker, markers inside
the program distinguish its operations from the enclosing native submission.
[Native power and residency](execution.md#runtime-power-and-residency)
explains why retaining host resources does not exclude wake-up cost.

### Timer registers and alignment

In AIE2IPU and AIE2P, each event module has timer control, low/high timer
values, and a programmable timer-event threshold. [AIE2IPU timer
modules][aie2ipu-timer-modules], [AIE2P timer modules][aie2p-timer-modules],
[threshold programming][aie-timer-thresholds]

`Timer_Control` can reset the timer directly or select an event that resets
it. [Direct reset][aie-timer-reset], [reset-event selection][aie-timer-reset-event]
A broadcast route can distribute one reset event to the participating modules.
`XAie_SyncTimer` implements this by configuring the broadcast network and reset
selectors for ungated tiles, generating the event, and clearing that temporary
configuration. [Timer synchronization][aie-timer-sync]

The AM025-derived shim register description places `Timer_Low` at local offset
`0x340f8` and `Timer_High` at `0x340fc`, each holding 32 bits. Those offsets
describe the register interface; a firmware timer-record operation has its own
sampling implementation. [Shim timer registers][shim-timer-registers]

The reset broadcast aligns the timers to a program event, not to a CPU clock.
Event propagation also takes time along the route. Cross-tile differences
therefore include any reset-arrival skew. A trace decoder needs the timer epoch
as well as the source tile and module to place samples on a shared timeline.

The register interface and the core instruction are different read paths.
`XAie_ReadTimer` performs separate low-word and high-word register reads; it
does not retry across rollover. The core `cntr` instruction returns a register
pair. The register address alone is not a host mapping. Register operations
require the native path appropriate to the platform, such as the executable's
controller instructions or control-packet routes. [Register-read
implementation][aie-timer-read]

AIE4 differs: its compute tile shares one timer between core and memory
modules. `XAie_ReadTimer` rejects a separate compute-tile memory-module read
for that architecture. The per-module AIE2IPU/AIE2P description does not
extend to that AIE4 case. [Architecture branch][aie-timer-read]

### Relating a sample to host time

An application can associate an in-program sample with a host interval. It
reads a monotonic host clock before native submission, executes a program that
reads `cntr` and stores the sample, then reads the same host clock after
successful command completion, result inspection and output acquisition. The
sample occurred between those host observations. The program's completion
covers the sample store and its transfer to the result destination.

That interval includes admission, execution, and completion observation. Its
midpoint is an estimate, not an exact host timestamp; half the interval width
is the corresponding uncertainty before accounting for clock resolution. Using
the sample to place other tile events on the host timeline also requires their
timer epoch and frequency history. A bracket for one tile does not align the
other tiles' timers.

XDP's edge collector uses this bracketing shape around `XAie_ReadTimer` and
records both host observations, tile coordinates, and the timer value. This is
a register-read collection path, separate from firmware `RECORD_TIMER` records
and from timer-reset broadcasts. [Edge timer sampling][xdp-edge-timers]

## Event counters

Performance counters are local to an event module. The AIE2IPU and AIE2P
device tables define the following resources. The counts describe hardware
counters, not a reservation of them for a profiling tool. [AIE2IPU
definitions][aie2ipu-registers], [AIE2P definitions][aie2p-registers]

| Module | Counters | Value width | Trace event slots |
| --- | --- | --- | --- |
| Compute-tile core | 4 | 32 bits | 8 |
| Compute-tile memory | 2 | 32 bits | 8 |
| Memory tile | 4 | 32 bits | 8 |
| Shim interface | 2 | 32 bits | 8 |

Counter control selects start, stop, and reset events. A separate value
register programs the threshold that generates the counter's own event.
Counter results are individual 32-bit register reads. Reset/configuration,
counting, and readback are separate operations. [Counter
operations][aie-counters]

Event identity determines the meaning of a count. Core activity, lock stalls,
stream stalls, vector instructions, and completed DMA tasks describe different
quantities. Stream-port events additionally use a selector identifying which
port is being observed; an event named `PORT_RUNNING_0` refers to a monitor
slot, not necessarily physical stream port zero. The Windows XDP profiler
programs those selectors and uses matching start/stop events to count selected
activity. [Event definitions][aie2p-events], [counter
configuration][xdp-profile]

Event numbers are module- and architecture-specific. For example, AIE2P core
instruction events 0 and 1 have event numbers 33 and 34; core user events 0
through 3 have numbers 124 through 127. An event emitted by an instruction and
one generated through `Event_Generate` are different event sources. [AIE2P
event definitions][aie2p-events]

### Returning counters through array streams

An AIE2P program can read its placement's counter registers through the array
control-packet protocol. A request contains a routing header and a control
word with the local register address, read operation, return packet ID, and
requested word count. The outgoing read request ends at the control word; the
requested words belong to the reply, not to the request payload. A read
returns a packet header followed by one to four 32-bit values. [Stream control
packets][stream-control]

The executable routes the request to the target's `TileControl` endpoint and
routes its reply to an ordinary shim S2MM destination. Reply routes preserve
the packet header and remain distinct from native DMA task-completion routes.
For example, reading the four core counter registers produces a 20-byte
packet. A 64-byte-capacity destination using finish-on-TLAST completes after
that packet; the controller waits for its task-completion token before
completing the command. Destination capacity is not the returned byte count.

For a program-defined work counter, matching `INSTR_EVENT_0` start/stop
selectors count the worker's `event 0` instructions. Initializing the counter,
emitting one event per work item, and reading after the last event produces a
count independent of host sampling intervals. Resetting the counter in each
command gives separate counts while reusing the same executable and
destination. A multi-word reply does not define a simultaneous snapshot of
running counters; the program quiesces their event sources when it needs
stable values.

This path uses the application's stream-switch and DMA resources, memory
visibility recipe, and execution-completion edge. It does not use the firmware
`READ_REGS` operation or require a context-associated debug buffer. Packet
construction and counter selection belong to the instrumented executable;
native submission carries that executable's controller program.

## Event trace

The trace unit watches eight configured event slots. Core trace supports
event-time, event-PC, and execution-trace modes. Memory and interface trace
units support event-time mode. In event-time mode, changes in the watched
events produce compressed trace frames. Event-PC records the program counter
and event state; execution trace records control-flow information for
reconstruction using the executable. [Hardware trace
modes][trace-architecture]

The configuration has four parts:

| Register group | Meaning |
| --- | --- |
| `Trace_Event0/1` | Maps the eight trace slots to hardware event numbers. |
| `Trace_Control0` | Selects the start event, stop event, and core trace mode. |
| `Trace_Control1` | Sets packet identity and type. |
| `Trace_Status` | Reports trace-unit state and mode, independently of DMA state. |

These are ordinary array register operations. The executable also configures
the packet-switched route from the trace port to its destination. Trace shares
stream-switch, DMA, and memory resources with application data. Several trace
sources can share an egress route and destination when packet routing
preserves their identity; a source does not inherently require its own shim
DMA channel. [Trace configuration][aie-trace], [trace
routing][trace-architecture]

The core `Trace_Status` register's State field is bits 9:8: zero means idle,
one means running, and three means overrun. Its Mode field is bits 2:0. These
are source-state observations; the register does not report downstream DMA
completion or an empty stream route. [Trace status fields][trace-status]

Start and stop selectors control collection independently of the eight event
slots. For example, a program can select `INSTR_EVENT_0` to start collection,
`INSTR_EVENT_1` to stop it, and `INSTR_STORE` in slot zero to observe stores.
The worker issues `event 0`, performs the operations being observed, and
issues `event 1`. Compiler scheduling places those operations strictly between
the markers. The resulting event frames name slot zero; their meaning comes
from its `INSTR_STORE` configuration, not from the start/stop event numbers.
[AIE2P event definitions][aie2p-events]

A finite capture uses an edge event for its start. `TRUE` is a level-sensitive
start condition: after a stop event makes the trace unit idle, the asserted
condition starts it again. It is suitable for continuous tracing, but not for
a one-shot interval. An explicit start instruction before the observed work
and a stop instruction after it define one interval without rearming.

The trace itself can carry the capture's terminal condition. For example, a
caller enables tracing with `INSTR_EVENT_0`, calls the observed function while
one trace slot watches `INSTR_RETURN`, and emits `INSTR_EVENT_1` after that
function returns. The return frame precedes the stop and partial-packet flush
in the same source stream. A consumer that parses through that slot therefore
knows it has received the end of the selected work; it does not infer the end
from an idle source register or an unwritten destination word.

### Packets and timestamps

The AIE-ML trace transport uses eight 32-bit words per packet: a routing
header and seven payload words. Payload words contain compressed trace frames,
not one event per word. Event-time frames refer to the configured trace slots,
so the decoder also needs the slot-to-event mapping. Filler occupies unused
framing space. [Trace packet layout][trace-architecture]

The MLIR-AIE event-time decoder separates packet headers from payload before
decoding events. Its start frame carries a seven-byte timer value; subsequent
frames advance the local timeline using cycle deltas. Its default `zero=True`
mode discards the starting timer value. That produces a relative per-stream
timeline, not an absolute cross-tile timeline. [Frame decoder][trace-decoder]

Long gaps have an additional encoding: the decoder's `Event_Sync` handling
advances the timer by `2^18` cycles without changing event state. Treating
that frame as padding loses time from the reconstructed interval. [Event-time
reconstruction][trace-reconstruction]

### Stop, flush, and DMA completion

A configured stop event ends collection at the trace source and flushes its
remaining partial packet. XDP's Windows `flushTraceModules` generates those
events for each traced module. It submits register operations, not a request
to complete the destination DMA. [Trace flush implementation][xdp-trace]

With length-based S2MM completion, the destination has its own programmed
transfer length. Stopping a source after it produces a short trace does not
change that length or complete an otherwise unfinished transfer.

An exact packet count allows one length-based transfer to collect several
packets and issue one task-completion token for the whole destination. For
example, two short, explicitly stopped windows that each produce one packet
fill a single 64-byte transfer, including their headers. No separate
allocation or descriptor is needed for each packet. The transfer length
follows the emitted packet count, not the number of selected events or the
allocation's maximum capacity: frames are compressed and packed together.

A shim DMA's channel status distinguishes an active transfer from tasks still
in its queue. `Channel_Running` covers both: zero means the datapath is idle
and the task queue is empty. Checking only `Task_Queue_Size` misses the active
transfer. Pause controls stop stream traffic or new memory requests; they are
not completion reports for a partially filled destination. [Shim DMA status
and control fields][aie-register-database]

AIE2IPU and AIE2P also support S2MM **finish on TLAST**. In that mode a
descriptor can complete when the incoming packet ends, before reaching its
configured buffer length. The channel selects how completed-transfer counts
are reported:

| Mode | Completion and count reporting |
| --- | --- |
| `DMA_FoT_DISABLED` | Length-based completion. |
| `DMA_FoT_NO_COUNTS` | Finish at TLAST, without queuing a word count. |
| `DMA_FoT_COUNTS_WITH_TASK_TOKENS` | Finish at TLAST, with counts accompanying task tokens. |
| `DMA_FoT_COUNTS_FROM_MM_REG` | Finish at TLAST, with counts read through the count FIFO register. |

`XAie_DmaChannelSetFoTMode` sets the channel description and
`XAie_DmaWriteChannel` programs it. The setting belongs to the array DMA
channel and remains independent of the destination allocation's capacity. [DMA
configuration][aie-dma], [AIE2IPU fields][aie2ipu-registers], [AIE2P
fields][aie2p-registers]

For example, a program can produce one trace packet into a descriptor with 64
bytes of capacity, finish after the packet's 32 bytes, and issue a task
completion token. A controller wait for that token covers the trace transfer
before native command completion. Retaining the packet header makes its source
identity and framing available in the destination. This uses ordinary trace
routing and DMA, without firmware result-buffer attachment.

TLAST is a packet boundary, not an end-of-capture marker: every eight-word
trace packet has one. In finish-on-TLAST mode a collector accounts for those
individual completions rather than treating the first token as completion of
the whole capture. Count-reporting modes add a FIFO that must be consumed; a
full count FIFO can stall the channel. The counts are 32-bit **words per
transfer**, not bytes or a cumulative capture length. The current-write-count
register observes an in-progress transfer and is not a retirement fence. [Shim
DMA register descriptions][aie-register-database]

The mode belongs to the DMA channel, not its buffer descriptors. A program
using length-based completion sets the mode to disabled when configuring that
channel; replacing a descriptor does not change the channel's completion rule.

The separation between source flush and DMA completion is visible in the
reference collectors. Windows XDP allocates and zeros a destination, programs
a shim S2MM descriptor for its capacity, then syncs and searches the storage
for the boundary between written data and zeros. MLIR-AIE inserts a stop-event
broadcast at the end of its runtime sequence; that insertion does not add a
wait for the trace DMA to consume its remaining capacity. These are collection
strategies, not a hardware-reported valid-byte count or a DMA-retirement
signal. [XDP offload][xdp-offload], [MLIR-AIE trace
insertion][trace-insertion]

### Fixed retirement for variable-length trace

A fixed-size in-array collector turns a variable packet count into ordinary
native work. It consumes complete eight-word packets, including each packet's
header and TLAST, until it parses the ordered terminal event. It copies
packets up to the destination's declared capacity, continues draining excess
packets while recording overflow, fills unused packet slots, and appends an
application-defined trailer. A maximum drain count bounds a missing terminal
while packets continue to arrive. The collector emits the trailer only after
the terminal packet or that packet-count bound. A source that stops producing
without its required terminal violates the executable's protocol; the
collector cannot infer completion from silence.

On an AIE2P compute tile, a trace source can drive the local stream-switch
FIFO, the south link, or DMA channel zero; it cannot directly drive a core or
the other cardinal links. A `TRACE -> FIFO` connection followed by a normal
circuit route lets a nearby collector core consume the packets without a
memory staging transfer. The FIFO changes only the legal switch connection;
the trace packet header and TLAST remain in the stream. [AIE2P stream-switch
connections][aie2p-stream-switch]

The collector's fixed output uses a length-based shim S2MM transfer. The
controller waits for that DMA task's ordinary completion token before native
command completion, so destination visibility and reuse follow the same
contract as any other execution output. There is no per-packet host action,
unused armed descriptor, destination scan, or second retirement mechanism. The
trailer layout, capacity, terminal-slot assignment, parser, and overflow
policy belong to the executable and its tooling; they are not fields of the
hardware trace-packet format.

Ordering is per trace source. Captures from several sources either retain a
terminal for each source or route through an explicit in-array join. A marker
from one tile cannot prove that another tile's final packet has already
entered the fabric.

XRT's PLIO collector has a different sink: a programmable-logic trace
datamover with written-word counters. Its version-2 final read resets the
datamover before reading those counters. That datamover is not the XDNA
array's shim DMA. Its flush and circular-buffer operations do not describe the
NPU's stream-to-memory completion contract. [PL trace
datamover][xdp-pl-datamover]

A controller completion used to retire trace storage follows the program's
join of its array work and transfers. A trace destination remains live while a
DMA can write it. A streaming program can publish completed regions to a
reader while continuing to produce into other regions; each region's
publication and reuse follow that program's protocol.

### Consuming completed-transfer counts

In `DMA_FoT_COUNTS_FROM_MM_REG` mode, each shim S2MM channel has a
completed-count FIFO. The AIE2P FIFO-pop registers are at local offsets
`0x1d238` and `0x1d23c` for channels 0 and 1. A report describes one completed
transfer:

| Field | Bits | Meaning |
| --- | --- | --- |
| `Valid` | 31 | This read returned a report. |
| `Last_in_Task` | 30 | This was the task's final transfer. |
| `BD_ID` | 29:24 | Descriptor used for the transfer. |
| `Write_Count` | 17:0 | Number of 32-bit words written to memory. |

With this mode selected, `Valid=0` means the FIFO is empty; the remaining bits
are unspecified, not necessarily zero. The same zero Valid bit is returned
when the channel is not in this mode. [Shim FIFO register
fields][aie-shim-fot-counts]

A control-packet consumer requests one 32-bit word at the selected FIFO-pop
register's exact address. Its reply is a packet header followed by that word.
Reading the surrounding aligned four-word block is not an equivalent consuming
operation. Consumption belongs to one reader: a diagnostic read cannot also be
treated as a passive copy for another consumer.

For a finite packet, the controller can wait for its DMA task-completion token
and then release a worker to read the completed count. The worker receives the
reply through its core stream and returns the report alongside numerical
output over a length-based, non-counting DMA channel. Returning it through the
count-producing channel would generate another transfer to account for. Shim
reply routing preserves the native task-completion route on the same
`TileControl` source. The controller waits for the output transfer before
completing the native command; the host then acquires the result through the
ordinary memory visibility recipe.

The report accounts for a completed transfer. It does not finish an unused
armed descriptor or identify the final packet of an entire trace capture.

## Firmware instrumentation

Firmware-generated results use a different path from application stores or
trace DMA. Two transaction operations use a destination associated with the
context:

| Operation | Input | Result |
| --- | --- | --- |
| `READ_REGS` | A list of array register addresses. | Register values copied into the firmware result buffer. |
| `RECORD_TIMER` | A caller-selected marker. | A marker and timer record appended to the firmware result buffer. |

The transaction definitions assign these opcodes `0x82` and `0x83`. The XDP
timeline reader consumes each timer record as three 32-bit words: ID, timer
high word, timer low word. The operation marks the interpreter's progress;
tile-work completion depends on the preceding controller synchronization. The
command and result carry no tile identity or clock-domain identifier; this
record is not a paired sample of the tile's `cntr` and a host clock.
[Transaction definitions][transaction-ops], [timeline result
reader][xdp-timeline]

DynamicDispatch's microcode converter assigns `OP_RECORD_TIMESTAMP` opcode
16 and dispatches it through `ExecRecordTimestamp`. When `__AIESIM__` is
absent and `_ENABLE_IPU_LX6_` is defined, that function passes its
`partBaseAddr` argument and the low 24 bits of the instruction word to
`RecordTimestampImpl`; it then advances by one word. The public wrapper
delegates the sample to that helper without defining timer selection, read
order or rollover handling. The marker encoding and the result-word layout
therefore do not establish an atomic sampling sequence. [Opcode
definition][dynamic-timer-opcode], [marker and helper call][dynamic-timer-call],
[dispatch][dynamic-timer-dispatch]

Converting records to durations requires the sampled timer's clock domain,
frequency history and epoch, including its behavior across idle, power gating,
reset and independent native lifetimes. Neither the opcode nor the three-word
result supplies those properties. A frequency written into a trace-file header
is format metadata, not a device contract. Host correlation still requires
explicit host bracketing and, for tile work, program-owned `cntr` samples.

### Linux context attachment

The AIE2 native path uses `DRM_AMDXDNA_HWCTX_ASSIGN_DBG_BUF` to associate an
`AMDXDNA_BO_DEV` buffer with a context, and `REMOVE_DBG_BUF` to detach it.
Attachment and detachment submit a native command and wait for its response.
The attachment argument names a BO, not a byte range. The driver supplies that
BO's heap-relative offset and full length to firmware through
`MSG_OP_CONFIG_DEBUG_BO`. Readback also uses a `SYNC_DEBUG_BO` command; an
ordinary host cache invalidate alone does not perform that synchronization.
[Context operations][linux-context], [firmware messages][linux-messages],
[shim buffer implementation][linux-buffer]

The AIE4 native interface uses the same configuration names with a different
payload: a metadata BO describes the result BO, buffer type, per-controller
slices, and a correlation tag. It is not the AIE2 BO-handle argument layout.
[Native configuration structures][linux-uapi]

### Windows client behavior

XDP creates a context-qualified `use_type::debug` buffer. Its counter profiler
submits `READ_REGS`, waits for the transaction, then synchronizes the result
buffer before reading it. Its timeline plugin retains a debug buffer across
execution, reads the accumulated timer records, and releases the buffer before
the counter/debug plugins install their result destinations. The public XRT
client sources describe this lifecycle; they do not define the private MCDM
allocation/attachment ABI. [Counter readback][xdp-profile], [timeline buffer
lifetime][xdp-timeline]

A normal trace DMA allocation does not acquire this firmware result-buffer
role. Attachment, firmware synchronization and detachment have their own
native context lifetime.

## Native clock information

Linux `DRM_AMDXDNA_QUERY_CLOCK_METADATA` returns two named integer-MHz values.
The AIE2 driver labels them `MP-NPU Clock` and `H Clock`; the AIE4 driver
labels the corresponding fields `NPU H Clock` and `AIE Clock`. The query
returns operating-clock information, not a counter sample. [Clock
fields][linux-clock-fields], [AIE2 clock query][linux-aie2-clock], [AIE4 clock
query][linux-aie4-clock]

With runtime PM enabled, the surrounding native query resumes the device and
holds a runtime-power reference, so polling it can affect idle behavior.
[AIE2 query lifetime][linux-aie2-query-power], [AIE4 query
lifetime][linux-aie4-query-power], [runtime PM condition][linux-query-runtime-pm],
[locked query entry][linux-query-runtime-pm-lock]

In the NPU4 register backend, the H-clock field is populated from the sensor's
NPU-clock reading; the MP-NPU field comes from the separate MP-NPU reading.
The field names therefore matter when interpreting the result. An operating
frequency sample does not record intervening frequency changes.
[Sensor-to-clock mapping][linux-npu4-clocks]

The firmware `CALIBRATE_CLOCK` command has a different purpose: the driver
sends `ktime_get_real_ns()` as a firmware time base and receives status. It
returns no paired tile/host sample. Firmware trace uses its own timestamp
mode: `FW_CHRONO` in the AIE2 path and `NS_OFFSET` in AIE4. [AIE2
messages][linux-messages], [AIE4 messages][linux-aie4-messages]

## Firmware diagnostic trace

The Linux firmware diagnostic channel is device-wide, separate from the trace
streams routed by an application. `SET_FW_TRACE_STATE` enables or configures
it through the root-only `SET_STATE` ioctl. Configuration queries are
unprivileged; payload reads require `CAP_SYS_ADMIN`. Multiple readers share
the channel's state. [IOCTL access][linux-ioctls], [channel
implementation][linux-dpt]

Payload reads carry a cursor and can block waiting for new data. `ESTALE`
reports that a disable/enable cycle invalidated the cursor and returns the new
cursor with zero payload. `ESHUTDOWN` reports a disabled channel and ends the
watch. With runtime PM enabled, a blocking read holds a runtime-power
reference while waiting. These notifications describe the diagnostic stream,
not application queue completion. [Payload protocol][linux-uapi], [AIE2 query
lifetime][linux-aie2-trace-query], [AIE4 query lifetime][linux-aie4-trace-query],
[runtime PM condition][linux-query-runtime-pm]

[cycle-counter]: https://download.amd.com/docnav/aiengine/xilinx2026_1/aiengine_ml_v2_intrinsics/intrinsics/group__intr__counter.html
[aie-timer-read]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/timer/xaie_timer.c#L327-L407
[aie2ipu-registers]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2ipugbl_reginit.c
[aie2p-registers]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c
[aie-dma]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma.c
[aie-register-database]: https://github.com/Xilinx/mlir-aie/blob/c69fb4c8f2fb853d5ca62d19f829796d3ae4ba34/lib/Dialect/AIE/Util/aie_registers_aie2.json
[aie-shim-fot-counts]: https://github.com/Xilinx/mlir-aie/blob/c69fb4c8f2fb853d5ca62d19f829796d3ae4ba34/lib/Dialect/AIE/Util/aie_registers_aie2.json#L75645-L75752
[aie-counters]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/perfcnt/xaie_perfcnt.c
[aie2p-events]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/events/xaie_events_aie2p.h
[aie2p-stream-switch]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/stream_switch/xaie_ss_aieml.c
[stream-control]: https://download.amd.com/docnav/aiengine/xilinx2025_1/aiengine_ml_v2_intrinsics/intrinsics/group__intr__streams__ms.html
[aie-trace]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/trace/xaie_trace.c
[trace-architecture]: https://docs.amd.com/r/en-US/am020-versal-aie-ml/Trace
[trace-status]: https://docs.amd.com/r/en-US/am025-versal-aie-ml-register-reference/Trace_Status-CORE_MODULE-Register?contentId=UVUMfHRQyoy4Sw9hZBAJSw
[trace-decoder]: https://github.com/Xilinx/mlir-aie/blob/c69fb4c8f2fb853d5ca62d19f829796d3ae4ba34/python/utils/trace/utils.py
[trace-reconstruction]: https://github.com/Xilinx/mlir-aie/blob/c69fb4c8f2fb853d5ca62d19f829796d3ae4ba34/python/utils/trace/parse.py
[trace-insertion]: https://github.com/Xilinx/mlir-aie/blob/c69fb4c8f2fb853d5ca62d19f829796d3ae4ba34/lib/Dialect/AIE/Transforms/AIEInsertTraceFlows.cpp
[xdp-trace]: https://github.com/Xilinx/XDP/blob/03ba80bf6c4942f51eebc71f7d154d9254426396/profile/plugin/aie_trace/client/aie_trace.cpp
[xdp-offload]: https://github.com/Xilinx/XDP/blob/03ba80bf6c4942f51eebc71f7d154d9254426396/profile/device/aie_trace/client/aie_trace_offload_client.cpp
[xdp-pl-datamover]: https://github.com/Xilinx/XDP/blob/03ba80bf6c4942f51eebc71f7d154d9254426396/profile/device/aieTraceS2MM.cpp
[xdp-profile]: https://github.com/Xilinx/XDP/blob/03ba80bf6c4942f51eebc71f7d154d9254426396/profile/plugin/aie_profile/client/aie_profile.cpp
[xdp-timeline]: https://github.com/Xilinx/XDP/blob/03ba80bf6c4942f51eebc71f7d154d9254426396/profile/plugin/ml_timeline/clientDev/ml_timeline.cpp
[xdp-edge-timers]: https://github.com/Xilinx/XDP/blob/03ba80bf6c4942f51eebc71f7d154d9254426396/profile/plugin/aie_trace/edge/aie_trace.cpp
[transaction-ops]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/common/xaie_txn.h
[linux-context]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_ctx.c
[linux-messages]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_message.c
[linux-buffer]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/src/shim/buffer.cpp
[linux-uapi]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h
[linux-npu4-clocks]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/npu4_regs.c
[linux-aie4-messages]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie4_message.c
[linux-ioctls]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_drm.c
[linux-dpt]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_dpt.c
[aie2ipu-timer-modules]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2ipugbl_reginit.c#L3875-L3917
[aie2p-timer-modules]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/global/xaie2pgbl_reginit.c#L4207-L4249
[aie-timer-thresholds]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/timer/xaie_timer.c#L74-L128
[aie-timer-reset]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/timer/xaie_timer.c#L149-L210
[aie-timer-reset-event]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/timer/xaie_timer.c#L235-L326
[aie-timer-sync]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/timer/xaie_timer.c#L882-L968
[dynamic-timer-opcode]: https://github.com/amd/DynamicDispatch/blob/b3051f03e20aab237cda3bbe4cd2081f76b72b06/src/txn/txn_utils.cpp#L738-L739
[dynamic-timer-call]: https://github.com/amd/DynamicDispatch/blob/b3051f03e20aab237cda3bbe4cd2081f76b72b06/src/txn/txn_utils.cpp#L1307-L1315
[dynamic-timer-dispatch]: https://github.com/amd/DynamicDispatch/blob/b3051f03e20aab237cda3bbe4cd2081f76b72b06/src/txn/txn_utils.cpp#L1773-L1776
[shim-timer-registers]: https://github.com/Xilinx/mlir-aie/blob/c69fb4c8f2fb853d5ca62d19f829796d3ae4ba34/lib/Dialect/AIE/Util/aie_registers_aie2.json#L77678-L77719
[linux-clock-fields]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/include/uapi/drm/amdxdna_accel.h#L408-L428
[linux-aie2-clock]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_pci.c#L742-L769
[linux-aie4-clock]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie4_pci.c#L975-L1002
[linux-aie2-query-power]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_pci.c#L819-L883
[linux-aie4-query-power]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie4_pci.c#L1174-L1244
[linux-aie2-trace-query]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie2_pci.c#L885-L965
[linux-aie4-trace-query]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/aie4_pci.c#L1298-L1377
[linux-query-runtime-pm]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_pm.c#L108-L134
[linux-query-runtime-pm-lock]: https://github.com/amd/xdna-driver/blob/8dfda66f67a84aecf26cf68336efc9e4cc1756c3/drivers/accel/amdxdna/amdxdna_pm.h#L20-L29
