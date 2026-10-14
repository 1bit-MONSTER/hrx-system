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
parity has changed. Six command variants restore both. AQLProfile duplicates
an odd-length backing sequence when its factory ID differs from the generic
`GFX9_GPU_ID`, and builds the replacement commands once. That predicate also
includes separately named MI factories; it is not an ISA-major comparison.
The GFX9 base-replacement operation itself has no slot-0/slot-1 operand.
[Backing sequence][backing-sequence]
[Prepared commands][prepared-commands]

The source-backed handoff contains more than a status read:

1. A separate control queue reads the selected SE/XCC's trace status into a
   control record, executes its barrier/cache sequence, and restores broadcast.
   The host observes completion before interpreting that snapshot.
2. A full-buffer indication chooses the next prepared replacement command.
   GFX12 writes the replacement base low word, executes the builder's barrier,
   then writes the high word. GFX9 updates BASE/BASE2. The command emits
   cache maintenance and restores broadcast.
3. The SDK waits for replacement-command completion before submitting the
   completed buffer's copy. It waits for that copy before publishing a CPU
   staging slot to the consumer.
4. The consumer releases its staging slot after its callback. A subsequent
   hardware replacement can reuse a GPU backing after the copy's last read;
   that GPU backing and the CPU staging slot have different final users.

[Status and replacement commands][live-builder]
[Producer sequence][live-consumer] [Copy dependency][copy-dependency]

The cache operand deserves separate attention. In the pinned constructor's
first replacement above, the helper receives B as its `prev` address while
the status consumer returns A for copying. The GFX12 cache builder emits
`GCR_CNTL = 0x18000`: forward sequencing and GL2 writeback with `GL2_RANGE =
ALL`. Its encoded address therefore does not restrict GL2 writeback to B.
A conversion to ranged maintenance cannot simply inherit that operand and
claim it describes the completed backing. The buffer-selection and visibility
contracts stay separate. [Prepared operands][prepared-commands]
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

## Overflow, discontinuities and decoding

The native full bits do not encode CPU-consumer credits. When CPU staging is
exhausted or the GFX12 loss indication is set, the SDK stops tracing, delivers
remaining data, emits a new stream header where applicable, and restarts.
Delivered chunks identify CPU-buffer-full or GPU-buffer-full discontinuities.
This continues collection with gaps; it is not a lossless backpressure promise.
[Overflow response][live-consumer]

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
[live-setup]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L300-L430
[aql-stop]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L526-L577
[live-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L654-L698
[service]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/service.cpp#L93-L204
[backing-count]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/aql/packet_construct.cpp#L200-L206
[caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/core.cpp#L267-L339
[backing-sequence]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L234-L262
[prepared-commands]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L394-L448
[live-consumer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L289-L343
[copy-dependency]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp#L82-L106
[idle9]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/gfx9_cmd_builder.h#L118
[barrier9]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/gfx9_cmd_builder.h#L61-L72
[barrier12]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/gfx12_cmd_builder.h#L279-L299
[cache12]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/gfx12_cmd_builder.h#L299-L334
[status-consumer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L358-L392
[phase-check]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/aql_packet.cpp#L250-L278
[final-reader]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L65-L127
[framing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L44-L58
[code-markers]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/threadtrace.cpp#L320-L349
[realtime-marker]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L636-L646
