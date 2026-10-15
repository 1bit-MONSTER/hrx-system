# Shader thread traces

SQ Thread Trace (SQTT) writes a selected stream of wave, instruction, register
and timing tokens into GPU memory. PM4 configures the trace units and their
buffers. A completed capture has a final write extent and error state; a live
capture additionally needs a protocol for replacing a completed buffer while
the trace writer continues. PAL supplies a finite-capture sequence, while
AQLProfile and ROCprofiler also implement source-specific live rotation.
[PAL start][pal11-start] [Live builder][live-builder] [Live consumer][live-consumer]

## Selection and representation

Trace selection includes physical SE placement, a CU or WGP/SIMD choice,
shader-stage selection, token filters and output storage. These are different
coordinates from a host queue or dispatch identifier. AQLProfile's GFX9 mask
builder uses its `vmIdMask` argument; the GFX11 and GFX12 builders accept that
argument but do not encode it. Their WGP, SIMD and compute-stage selections
alone do not establish process isolation. Native access and other state
owners belong to the [profiling ownership contract](../observability.md#counter-ownership-and-interpretation).
[GFX9 mask][mask9] [GFX11 mask][mask11] [GFX12 mask][mask12]

The cited AQLProfile builder uses 4 KiB address/size units and 32-byte write
pointer increments. Its per-SE allocation policy accepts at least 128 KiB and
less than 16 GiB, with a smaller limit of less than 512 MiB on its GFX11 path.
Those allocation limits are the builder's checks, not an independent statement
of every hardware field's range. [Units][builder-units]
[Allocation checks][builder-limits]

| Source path | Buffer representation and live-rotation mechanism |
| --- | --- |
| AQLProfile GFX9 | `SQ_THREAD_TRACE_BASE`, `BASE2` and `SIZE`; `MODE.WRAP` enabled for live rotation. The builder programs an initial address, enables tracing, then programs the next address. Replacement updates BASE/BASE2. |
| PAL GFX11 finite capture | `SQ_THREAD_TRACE_BUF0_SIZE` contains size and upper base bits; `BUF0_BASE` supplies the lower base. Size is written before base, and `CTRL` is written last to enable tracing. |
| AQLProfile GFX10/GFX11 | Its multibuffer setup throws an unsupported-mode error. Register names or a wrap field elsewhere do not establish an equivalent supported sequence. |
| AQLProfile GFX12 | Separate `BUF0` and `BUF1` size/base registers, `CTRL.DOUBLE_BUFFER`, and full-buffer status in `STATUS2`. GFX12.0 and GFX12.50 use distinct primitive/register variants. |

[GFX9 wrap configuration][control9] [GFX11 setup][pal11-start]
[Builder branches][live-setup] [GFX12 primitive variants][variants12]

The pinned PAL GFX12 definitions give these buffer and completion bits:

| Register | Bits | Meaning used by the cited builders |
| --- | --- | --- |
| `SQ_THREAD_TRACE_CTRL` | 5 | `DOUBLE_BUFFER`. |
| `SQ_THREAD_TRACE_STATUS2` | 0, 1 | `BUF0_FULL`, `BUF1_FULL`. |
| `SQ_THREAD_TRACE_STATUS2` | 4 | `PACKET_LOST_BUF_NO_LOCKDOWN`. AQLProfile treats this as a late-rotation loss indication. |
| `SQ_THREAD_TRACE_STATUS` | 11:0 | `FINISH_PENDING` field. |
| `SQ_THREAD_TRACE_STATUS` | 23:12 | `FINISH_DONE` field. PAL waits for a nonzero value under this mask. |
| `SQ_THREAD_TRACE_STATUS` | 24, 25 | `WRITE_ERROR`, `BUSY`. |

[Register masks][masks12] [AQLProfile status interpretation][control12]

## Finite capture and final trace writes

PAL's GFX11 and GFX12 compute stop sequences clear
`COMPUTE_THREAD_TRACE_ENABLE`, send `THREAD_TRACE_FINISH`, and visit each
selected SE. For each instance they wait for `FINISH_DONE`, set trace mode
OFF, and wait for `BUSY` to clear before copying the final information words.
The graphics path uses `THREAD_TRACE_STOP` for the first step. GFX12 explicitly
closes its firmware programming window around the stop/finish event sequence
and reopens it for indexed access. [GFX11 stop][pal11-stop]
[GFX12 stop][pal12-stop]

The three copied PAL information DWORDs are write pointer, status and dropped
counter. Their completion establishes a source-specific final extent; cache
visibility, command retirement and the consumer's final read still belong to
the surrounding submission. PAL's GFX11 write-pointer workaround subtracts
the buffer base in the pointer's 32-byte units because the raw field contains
address bits rather than a buffer-relative position. Its subtraction uses
the 29-bit field modulus. The accompanying source explicitly says a
double-buffer implementation would also need to preserve the buffer-ID field.
[Information copy and pointer correction][pal11-info]

AQLProfile uses its own stop builder and returned `TraceControl` layout. Its
GFX12 path sends FINISH, waits for FINISH_PENDING to clear, disables the trace,
waits for BUSY to clear, then reads control fields. The GFX11 branch in that
builder does not emit the same FINISH packet as PAL. These are distinct
consumer sequences; combining their individual steps does not establish a
new stop contract. [AQLProfile stop][aql-stop]

## Live buffer replacement

The real ROCprofiler caller selects live rotation in device-trace mode,
rejects it in dispatch-trace mode, and restricts it to one SE. Its public
buffer-count parameter chooses CPU staging slots: one is the synchronous
path, two is rejected, and at least three selects its asynchronous collector.
The command factory independently requests **three GPU backing buffers**.
These are runtime choices, not a requirement for the application to create one
thread per hardware buffer. [Service selection][service]
[GPU backing count][backing-count] [Actual caller][caller]

The SDK's embedded AQLProfile path can use direct KFD queues, memory and
signals, or the ROCr/HSA transport selected by its resource factory. The
selection includes a native device check and a supported-target predicate;
the existence of an AQL-shaped packet structure does not identify the queue
that ultimately executes its commands. Both paths retain a separate control
queue for live collection. [Transport selection][queue-source]

For the GFX12 path, two hardware buffer slots rotate through those three
backings. With A initially in slot 0 and B in slot 1, the replacement schedule
is:

| Completed backing | Slot reprogrammed | New backing | Backing available for collection after the handoff |
| --- | --- | --- | --- |
| A | 0 | C | A |
| B | 1 | A | B |
| C | 0 | B | C |
| A | 1 | C | A |
| B | 0 | A | B |
| C | 1 | B | C |

After three replacements the backing order repeats but the hardware-slot
parity has changed. Six command variants restore both. The embedded AQLProfile
duplicates an odd-length backing sequence for factory IDs at or above
`GFX10_GPU_ID`, and builds the replacement commands once. Its preceding GFX9
and MI factory IDs use one round. The GFX9 base-replacement operation itself
has no slot-0/slot-1 operand.
[Backing sequence][backing-sequence]
[Prepared commands][prepared-commands]

The successful handoff contains more than a status read:

1. A separate control queue reads the selected SE/XCC's trace status into a
   control record, executes cache maintenance, and restores broadcast.
   The host observes completion before interpreting that snapshot.
2. A full-buffer indication chooses the next prepared replacement command.
   The embedded GFX12 builder writes the replacement base low word followed
   by the high word through unconfirmed `COPY_DATA` register writes; it emits
   no intervening partial flush. GFX9 waits for `BUSY` to clear before updating
   BASE/BASE2. The command emits cache maintenance and restores broadcast.
3. The SDK waits for replacement-command completion before submitting the
   completed buffer's copy. It waits for that copy before publishing a CPU
   staging slot to the consumer.
4. The consumer releases its staging slot after its callback. A subsequent
   hardware replacement can reuse a GPU backing after the copy's last read;
   that GPU backing and the CPU staging slot have different final users.

[Status and replacement commands][live-builder]
[Register-write encoding][register-write12]
[Producer sequence][live-consumer] [Copy dependency][copy-dependency]

The replacement constructor supplies the completed backing as the cache
operand: in the first replacement above, both that operand and the status
consumer's copy source are A. The GFX12 cache builder emits
`GCR_CNTL = 0x18000`: forward sequencing and GL2 writeback with `GL2_RANGE =
ALL`. The encoded address therefore does not restrict GL2 writeback to A.
The buffer-selection and visibility contracts stay separate.
[Prepared operands][prepared-commands]
[Returned backing][status-consumer] [Cache encoding][cache12]
[Range fields](../pm4/cache.md#acquire-gcr-fields)

The helpers named `BuildWriteWaitIdlePacket` encode `CS_PARTIAL_FLUSH` on the
cited GFX9/GFX12 compute builders. This is a dependency in the sampling command
stream, not a universal join of every workload queue. Placing those commands
behind a resident dispatch in the same queue changes their ability to make
progress; the actual live caller uses its separate control queue.
[GFX9 helper][idle9] [GFX9 barrier][barrier9]
[GFX12 barrier][barrier12] [Collector queue][caller]

`aqlprofile_att_update_buffer_status` interprets a cached status record and
increments its software swap count when that record says full. It is therefore
a state transition, despite its query-like name. Reinterpreting the same
snapshot twice can advance that counter twice. The caller's swap generation,
status-query completion and buffer-address phase remain part of the protocol.
[Status consumer][status-consumer] [Caller phase check][phase-check]

## Copy completion and native queue retirement

In the successful replacement sequence above, command completion retires the
trace writer's use of the collected backing. A subsequent transfer still reads
that backing and writes a CPU staging slot. Transfer completion permits reuse
of the GPU backing; the CPU consumer's last read separately permits reuse of
its staging slot. Stopping the trace does not replace either transfer or
consumer completion.
[Producer ordering][transfer-producer] [Consumer release][transfer-consumer]

The SDK's copy wrapper returns a Boolean. Its HSA path reports rejected
asynchronous copies and waits for accepted copies; its direct KFD path rejects
invalid requests before publication and otherwise waits for the SDMA fence.
However, the producer discards that return value and publishes the requested
byte count with `filled = true` even after failure. That software readiness
flag therefore does not prove that the slot contains the requested payload.
The successful-copy sequence above and this failure path have different data
validity guarantees. [Copy wrapper][transfer-wrapper]
[Direct copy][direct-copy] [Slot publication][transfer-publication]

Direct KFD signal and ring-space waits use a loop with a 30-second diagnostic
interval. The interval is a logging cadence, not a timeout or a native failure
observation. The wait terminates only when its memory predicate changes.
[Wait implementation][direct-wait] [Signal wait][direct-signal]

The direct collector attempts SDMA and AQL queue removal before destroying
command, signal and trace allocations. Each native queue close reports failure
and leaves its software `created` flag set, but the collector's destruction paths
do not use that result to gate deallocation. The ordering of destructor calls
alone therefore supplies neither successful queue removal nor acknowledgement
that a failed trace writer has stopped. Normal trace drain, transfer retirement
and native queue removal remain separate observations.
[Collector destruction][direct-agent-destroy]
[Native queue close][direct-close] [Transfer owner][direct-owner-close]
[Staging destruction][direct-staging-destroy]

## Clock anchors and chunk timing

The embedded AQLProfile adds two 64-bit fields to its `TraceControl` record:
`gpu_clock_start` and `gpu_clock_latest`. Clock fields are written once per
selected XCC; the reader maps an SE to its XCC using `SE / SEs_per_XCC`.
The same record array also carries per-SE trace status, so the two uses have
different index meanings. Allocation initializes the clock fields to zero.
Reading these fields does not itself wait for a producer or sample a new
clock. [Control representation][clock-layout]
[Initialization][clock-initialize] [Reader][clock-reader]

| Command point | Captured anchor |
| --- | --- |
| Start | After enabling the selected trace units and writing decoder metadata, capture `gpu_clock_start` for each participating XCC. |
| Buffer replacement | After writing the new base, capture `gpu_clock_latest` for that XCC, then perform payload and control-record cache maintenance. |
| Stop | Capture `gpu_clock_latest` before disabling or finishing the trace and waiting for its final writes. |

These anchors identify command-processing points. A replacement timestamp is
not the time the previous buffer filled, and the stop timestamp precedes final
trace retirement. The interval attached to a chunk therefore does not establish
the first and last token's exact execution times or a bound containing every
token. The token decoder retains its own timing context.
[Start and stop placement][clock-placement] [Replacement placement][live-builder]

The GFX9 path uses a confirmed 64-bit `COPY_DATA` from `GPU_CLOCK_COUNT` to
memory. When realtime markers are enabled, it also copies that captured value
into trace userdata and emits a fresh low-word clock marker. The GFX11 and
GFX12 builders use a confirmed 64-bit copy to `TC_L2`; the same clock-field
interface does not imply identical memory policies or a trace-token marker on
every family. [Clock selection][clock-selection]
[GFX9 capture and marker][clock9] [GFX11 capture][clock11]
[GFX12 capture][clock12]

The live caller waits for a completed status query before reading the initial
anchor, waits for replacement completion before reading the next anchor, and
waits for stop before collecting the final pair. A restart clears the caller's
cached starting anchor; it reads the new one after the next completed query
or stop. This avoids reusing an earlier capture's clock while the restart
packet is still pending. [Live timing and restart][live-consumer]
[Final pair][clock-final]

Each delivered chunk can carry both raw GPU ticks and a translated system
clock. The SDK retains the raw value and calls ROCr's
`hsa_amd_profiling_convert_tick_to_system_domain` separately for each anchor.
Missing HSA-agent association, unavailable conversion, or conversion failure
leaves the system field zero. In particular, a direct KFD capture can have
useful raw anchors without an available HSA conversion. Separate scalar
conversions do not supply a shared calibration snapshot or an uncertainty
bound. [Conversion][clock-convert]
[Host relation](../observability.md#host-brackets-and-uncertainty)

## Overflow, discontinuities and decoding

The native full bits do not encode CPU-consumer credits. The embedded SDK
distinguishes two exhaustion paths:

| Observation | Consumer response |
| --- | --- |
| CPU staging has no free slot after a successful GPU replacement. | Submit and wait for stop, wait for a CPU slot, deliver the retired backing and final tail, emit a new header where applicable, then restart. Chunks identify the CPU-buffer-full discontinuity. |
| The GPU lockdown-failure indication is set. | Disable submission on the control queue and return without swap, stop, restart or later code-object markers. Discard GPU-resident data and deliver only already-copied CPU slots. |

The first path continues collection with gaps. The second deliberately avoids
further trace-control packets; the software queue's disabled state is not a
hardware stop acknowledgement. The normal `FINISH`/`BUSY` drain contract cannot
be inferred from that error path. Storage retirement after a failed trace
writer needs its native recovery contract independently of the collector's
thread join. [Overflow response][live-consumer]
[Collector exit][collector-exit] [Agent teardown][agent-teardown]

GFX9's cited primitive reports the wrapped-status mask but supplies no
lockdown-failure mask. GFX12 supplies the full-buffer and packet-loss masks.
Neither a wrapped pointer nor a single loss flag gives an exact number of
missing records. AQLProfile's final reader checks native write errors, bounds
the final byte extent by the allocation, and selects the last backing using
its software rotation count. [GFX9 status][control9]
[GFX12 status][control12] [Final reader][final-reader]

Trace tokens also require decoding context. AQLProfile adds a synthetic GFX9
header identifying the source and buffering mode, code-object load/unload
markers carrying addresses, sizes and IDs, and optional realtime markers.
Those software records are distinct from the native token stream and the
control-buffer format. Trace-clock reconstruction and code-object lifetime
need that context; a byte buffer alone is not a resolved instruction timeline.
[Decoder framing][framing] [Code-object markers][code-markers]
[Realtime marker][realtime-marker]

[pal11-start]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2975-L3025
[pal11-stop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3084-L3152
[pal11-info]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3153-L3204
[pal12-stop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PerfExperiment.cpp#L2662-L2760
[masks12]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_mask.h#L6340-L6388
[mask9]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/gfxip/gfx9/gfx9_primitives.h#L556-L566
[mask11]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/gfxip/gfx11/gfx11_primitives.h#L538-L550
[mask12]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/gfxip/gfx12/gfx12_primitives.h#L497-L511
[variants12]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/gfxip/gfx12/gfx12_primitives.h#L38-L59
[builder-units]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L155-L163
[builder-limits]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L215-L233
[control9]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/gfxip/gfx9/gfx9_primitives.h#L667-L730
[control12]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/gfxip/gfx12/gfx12_primitives.h#L616-L640
[live-setup]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/sqtt_builder.h#L320-L499
[aql-stop]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/sqtt_builder.h#L541-L637
[live-builder]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/sqtt_builder.h#L754-L829
[service]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/service.cpp#L113-L228
[backing-count]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/aql/packet_construct.cpp#L206-L211
[caller]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/core.cpp#L345-L425
[backing-sequence]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/threadtrace.cpp#L275-L302
[prepared-commands]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/threadtrace.cpp#L492-L559
[live-consumer]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L307-L389
[copy-dependency]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/hsa_util.cpp#L271-L293
[idle9]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/gfx9_cmd_builder.h#L118
[barrier9]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/gfx9_cmd_builder.h#L61-L72
[barrier12]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx12_cmd_builder.h#L345-L364
[cache12]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx12_cmd_builder.h#L366-L400
[status-consumer]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/threadtrace.cpp#L421-L463
[phase-check]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/aql_packet.cpp#L273-L301
[final-reader]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/threadtrace.cpp#L69-L166
[framing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L44-L58
[code-markers]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L320-L349
[realtime-marker]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L636-L646
[queue-source]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/core.cpp#L165-L204
[register-write12]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx12_cmd_builder.h#L170-L229
[collector-exit]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L294-L305
[agent-teardown]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/core.cpp#L206-L225
[clock-layout]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/sqtt_builder.h#L92-L102
[clock-initialize]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/threadtrace.cpp#L305-L310
[clock-reader]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/threadtrace.cpp#L466-L488
[clock-placement]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/sqtt_builder.h#L527-L555
[clock-selection]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/sqtt_builder.h#L717-L743
[clock9]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx9_cmd_builder.h#L550-L633
[clock11]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx11_cmd_builder.h#L508-L523
[clock12]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx12_cmd_builder.h#L804-L819
[clock-final]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L239-L274
[clock-convert]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L44-L68
[transfer-producer]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L339-L390
[transfer-consumer]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L95-L147
[transfer-wrapper]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/hsa_util.cpp#L270-L296
[direct-copy]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/resource.cpp#L1060-L1087
[transfer-publication]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L203-L237
[direct-wait]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/resource.cpp#L87-L104
[direct-signal]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/resource.cpp#L702-L736
[direct-agent-destroy]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/core.cpp#L206-L227
[direct-close]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/resource.cpp#L859-L882
[direct-owner-close]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/resource.cpp#L1135-L1143
[direct-staging-destroy]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/hsa_util.cpp#L210-L233
