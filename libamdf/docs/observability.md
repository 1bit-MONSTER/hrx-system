# Timing and observation

libamdf exposes native observation sources, their storage contracts, and their
clock metadata. The caller owns workload identity, queues, result storage,
collection, and the relationship between device and host clocks. Profiling
does not introduce a library-owned collector or change ordinary submission.

The shared [observation types](../include/amdf/observation.h) describe a clock
and the progress of a native read. Source-specific operations live in the
[GPU](../include/amdf/gpu.h) and [XDNA](../include/amdf/xdna.h) extensions.
Extension negotiation establishes that an API table is present. The selected
queue family, live device, context, or native service establishes which
operations are available.

The device-independent `AMDF_EXTENSION_OBSERVATION` table supplies the profiling
host clock. `query_host_clock_info` returns its frequency and counter width;
`query_host_clock` reads its ticks. Neither requires an instance or device.
CPU intervals and the host anchors for every GPU/NPU source can therefore use
one portable timebase without calling native clock APIs. The caller queries
units once and subtracts timestamps before converting a duration:

```c
const void* extension = NULL;
amdf_status_t status = api->query_extension(
    AMDF_EXTENSION_OBSERVATION, AMDF_OBSERVATION_EXTENSION_VERSION_1,
    AMDF_OBSERVATION_EXTENSION_VERSION_LATEST, &extension);
// Handle status before consuming extension or any subsequent output.
const amdf_observation_api_t* observation = extension;
amdf_observation_clock_info_t host_clock = {0};
status = observation->query_host_clock_info(&host_clock);
uint64_t before = 0, after = 0;
status = observation->query_host_clock(&before);
// Perform CPU work, or publish and observe the intended device work.
status = observation->query_host_clock(&after);
double duration_ns = (double)(after - before) * 1e9 / host_clock.frequency_hz;
```

The host clock is independent of wall-clock adjustments. Its epoch is shared
by callers on that host within one boot, including separate provider instances.
It does not establish a common epoch between network nodes. Suspend, device
reset, and observation-source loss require refreshing device/host relations.

## Selecting a source

| Observation | Selection and output | Readiness and reuse |
| --- | --- | --- |
| CPU submission and completion observation | Observation extension host-clock reads around the caller's actual publication and completion operations. | Measures host-observed latency, including scheduling and observation delay. It does not supply device execution timestamps. |
| GPU PM4 or AQL-carried command-processor clock | Caller-written native timestamp commands and destination storage. `device_query_queue_clock_info` supplies nominal units for the selected queue family. | The command's pipeline stage and explicit dependencies determine what it measures. Result-write completion and the memory edge precede collection. |
| GPU SDMA clock | Caller-written `TIMESTAMP_GET_GLOBAL` commands around the intended transfer work. The same queue-clock query supplies nominal units. | The caller orders payload release and completion after the ending sample, then acquires the result before reuse. |
| AQL dispatch interval | An AQL family advertises `DISPATCH_TIMESTAMPS`; queue creation selects `AMDF_GPU_USER_QUEUE_FLAG_DISPATCH_TIMESTAMPS`. Firmware writes the dispatch's native completion signal. | Each observed dispatch owns an exclusive signal through completion and timestamp consumption. Capture is a queue-lifetime option, while the caller chooses which dispatches receive observation signals. |
| XDNA transaction marker | `context_query_result_buffer_info` supplies destination geometry. The caller attaches backing and supplies `RECORD_TIMER` instructions and the finite record protocol. | Every result writer retires before native collection and host cache acquire. Valid extent follows the caller's record protocol. |
| XDNA automatic firmware events | `device_query_firmware_trace_info` reports recorder state, schema, clock, and read-storage requirements. `device_read_firmware_trace` returns decoded events incrementally. | Native transport can lose data. Read flags and native byte positions describe progress; neither an event nor an empty read retires application work. |

The [GPU timing contract](gpu.md#queue-timing) describes explicit timestamps
and AQL signal ownership. [XDNA result buffers](xdna.md#finite-firmware-results)
and [automatic firmware timing](xdna.md#automatic-firmware-timing) describe
the two distinct NPU sources.

PM4/AQL queue availability follows the native provider. AQL is a Linux service;
Windows GPU queues retain their PM4/SDMA mechanisms. XDNA automatic events use
the native MCDM service on Windows and the SDK DPT interface when supplied by
the Linux driver. An absent trace service returns `UNSUPPORTED`. A permission
failure remains a native error and is not evidence that the hardware lacks
the source. Finite transaction markers have their own admission independently
of that recorder.

## Clock units and correlation

`amdf_observation_clock_info_t` carries `frequency_hz` and meaningful
`counter_bit_count`. Zero means the corresponding fact is unspecified. The
source and its capture lifetime identify the clock; the record is not a
globally comparable clock identifier. Equal frequencies, a shared device,
and a common storage width do not establish a shared epoch.

GPU extension version 2 exposes the native nominal queue-clock rate. It is
separate from shader-cycle and performance-event units. XDNA format-2 automatic
events represent nanoseconds. XDNA extension version 2 exposes the live
partition-shim operating rate for `RECORD_TIMER`; the cached result-buffer
geometry deliberately leaves that rate unspecified. These queries allocate
no collector and retain no calibration history.

A same-epoch, non-wrapping delta at a constant rate converts to nanoseconds as
`delta_ticks * 1e9 / frequency_hz`. Subtract the source origin before converting
to floating point or widening the arithmetic. A rate describes units; it does
not establish oscillator accuracy or continuity through gating, preemption,
reconfiguration, or reset. For the XDNA array timer, equal rate snapshots before
and after work do not prove that the intervening rate stayed constant.

A caller can bound a source-to-host offset with an independently attributed
interval. If `host_before` precedes its submission and `host_after` follows
its completion, and the source endpoints have been converted to nanoseconds,
a constant-offset model gives:

```text
host_before - source_begin <= offset <= host_after - source_end
```

The interval width includes scheduling and host-observation delay. Its midpoint
is an estimate with half that width as sampling uncertainty, not an exact
calibration. Intersecting bounds from several observations can narrow a stable
segment; an empty intersection exposes inconsistent attribution, rate, or
epoch. A long capture needs a relation that accounts for drift and invalidates
old segments after loss or reset. Each GPU/NPU source gets its own relation to
the caller's chosen host clock before events from different sources are
compared.

## Sustained collection and storage

Caller-controlled GPU observations can use a fixed ring of slots. The producer
obtains a free slot, writes its observation, establishes the required visibility,
and publishes that generation. The consumer acquires the matching generation,
copies the record to its sink, and returns the slot. Work can continue on other
slots throughout collection. Queue-ring consumption permits command-storage
reuse; it does not replace the observation's result-publication boundary.

AQL dispatch timing uses the dispatch completion signal as that publication
boundary. Its timestamp fields remain borrowed until the caller has consumed
them. Each dispatch carries its own signal; a shared counter or a later barrier
cannot preserve individual intervals. A resident dispatch supplies only its
outer interval. Internal regions require an observation source that runs while
that dispatch remains live.

[Memory reservations](memory.md#stable-reservations-and-replaceable-backing) can project one
backing range repeatedly into a contiguous address range when the selected
memory profile supports it. Such aliases simplify wraparound access; they do
not create publication, credits, or overwrite protection. Native record headers
and payload stride still determine the source's actual ring geometry.

Automatic XDNA trace reads instead copy from a lossy native recorder into
caller-provided host storage. Each reader owns a position and a capture
generation. A successful `amdf_observation_read_result_t` identifies the valid
output extent and next native position. `GAP`, `RESTART`, and `DISCARDED` are
processed before the batch's records; interval pairing cannot cross those
discontinuities. Decoded output bytes and native cursor units can differ.
Storage is borrowed only during the read and may change even on failure;
failure leaves the result descriptor unchanged.

Finite XDNA result buffers have a different lifecycle. The caller budgets every
record, publishes its destination, attaches it, and runs the planned commands.
After all result writers retire, it collects and acquires the host view before
consuming records. Detachment ends the native association; memory remains
caller-owned. Neither an append position nor a live-read cursor is supplied by
this mode, so it cannot be consumed as the automatic trace stream.

## Native controls and teardown

XDNA firmware recording is device-wide. Its configuration owner arranges
cross-process authority and serializes reconfiguration against reads. The
library does not acquire that authority, and device destruction does not
disable the recorder. An external privileged recorder can own configuration
while a workload remains unprivileged, subject to the reader's independent
native permission requirements.

Normal teardown follows the source's last writer and last reader. GPU slots
are released after their producers retire and their records are consumed.
Finite XDNA result backing stays live until its writers retire and its native
association is removed. A failed native control can have made progress; its
error is not rollback or permission to free borrowed backing. Automatic trace
storage is borrowed only for each read, while final event draining follows the
application's completion and expected-event protocol. An empty batch alone
does not establish that all events have arrived.
