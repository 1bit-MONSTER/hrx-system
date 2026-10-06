# AQL timing and counter ownership

AQL dispatch profiling records packet-processing timestamps in native
completion-signal storage. Explicit command-processor clock reads and
programmable counter collection use different commands and ownership
protocols. Their results become meaningful only with the capture state,
execution boundary, clock or event definition, and final storage user.
[Profiling API contract][profile-api] · [Native signal layout][signal-abi]

## Command-processor clock capture

AQLProfile's GFX9 `ClockRetrievePacket` emits a confirmed six-DWORD COPY_DATA
from the GPU clock to memory:

| DWORD | Field |
| --- | --- |
| 0 | COPY_DATA header `0xc0044000`. |
| 1 | Control `0x02112509`: clock source 9, MEMORY destination 5, STREAM source/destination policies at bits 13/25, 64-bit count at bit 16, write confirmation at bit 20. |
| 2–3 | Zero; the clock source has no memory-address operand. |
| 4–5 | Eight-byte-aligned destination address, low/high words. |

[Clock packet builder][clock-packet]

Its clock-marker caller selects virtual XCCs explicitly and gives each sample
separate output storage. A [vendor-format-1 carrier](transfers.md) can execute
the buffer; native completion protects the borrowed IB independently of the
memory command's write confirmation. Predication chooses an executor, not a
storage-reclamation boundary. [Per-XCC caller][clock-routing] · [Carrier
completion][pm4]

COPY_DATA samples command-processor progress. It does not implicitly wait for
previous shader or DMA work to finish. PAL chooses different timestamp
commands for CP and bottom-of-pipe stages and separately drains CP DMA when
needed. A workload interval consequently needs explicit dependencies that
place the work between the intended sampling stages. [PAL stage
selection][pal-timestamp]

A complete capture keeps the command buffer, destination, and native signal
live, publishes them for the queue's mappings, submits the native carrier, and
acquires its completion before reading or reusing sample storage. The
carrier's selected release and the host acquire retain their
payload-visibility roles. Equal successive samples can be valid; a 64-bit
destination and write confirmation do not establish effective counter width,
nonzero elapsed time, physical frequency accuracy, or atomicity of arbitrary
concurrent reads. [Clock builder][clock-packet] · [Native join][pm4] · [Clock
domains](../observability.md#clock-domains-and-conversion)

## Dispatch timestamps

| Native state | Contract |
| --- | --- |
| Queue enable | `AMD_QUEUE_PROPERTIES_ENABLE_PROFILING` is bit 3 in `queue_properties`. |
| Result storage | `amd_signal_t.start_ts` and `end_ts` are unsigned 64-bit fields at bytes 32 and 40 of the 64-byte signal block. |
| Observation | A completed dispatch from a profiling-enabled queue owns an unreused completion signal until its timestamps have been read. |
| Conversion | ROCr converts agent ticks into the HSA system-clock domain using correlated samples. |

[Queue ABI][queue-abi] · [Signal ABI][signal-abi] · [Observation
contract][profile-api]

The API describes packet-processing start and packet completion. It does not
define first/last shader instruction, isolated ALU time, or host submission
latency. These sources do not specify the precise firmware sampling point
relative to dispatch acquire/release, or how a multi-XCC dispatch contributes
to the two fields. A barrier completion signal alone does not imply dispatch
timestamp capture. [Timestamp definition][profile-api]

ROCr's enable entry resolves its own queue and calls `AqlQueue::SetProfiling`.
If the property changes after the write index becomes nonzero, the method
suspends and resumes the queue so CP reloads it. The condition is the write
index, rather than current idleness. Repeating the same setting does not
perform this remap; enabling also initializes clock-correlation progress. CLR
enables capture while constructing its queue, before returning it. [API
entry][api] · [Reload condition][enable] · [Property setter][setter] · [CLR
queue policy][clr-enable]

An ordinary ROCr caller therefore creates a queue, enables profiling, assigns
an exclusive completion signal to a valid dispatch, acquires completion, and
queries time with the same agent and unreused signal before releasing them.
The getter requires ROCr-owned objects with their associated metadata; an
independently initialized native ABI block is not such an object. [Getter and
object validation][api] · [Signal metadata][signal-owner]

The getter checks object validity, but does not check that capture was
enabled, the dispatch completed, or the signal stayed unreused. Its translator
returns zero start/end if either raw value is zero or predates initial
calibration, while the getter can still return success. Ordinary signal-value
stores do not clear timestamp fields. Result identity and completion are thus
obligations of the owner, rather than facts implied by API success or value
rearming. [Getter][api] · [Invalid timestamps][translate] · [Signal
stores][signal-stores]

This queue property does not acquire KFD's programmable-counter owner. Its
reload and correlation operations are separate from that permission and
lifecycle. [Profiling update][enable] · [KFD PMC admission][pmc-admission]

## Clock domains and partitions

`GpuAgent::TranslateTime` translates the end first so a resampling step does
not change scale midway through an interval. It rejects invalid raw values,
limits extrapolation, updates correlated samples, and handles drift. GPU
wallclock frequency comes separately from topology `WallClockKHz` or a driver
query; it is not the instantaneous shader operating frequency.
[Translation][translate] · [Frequency initialization][frequency]

The Windows path also retains an offset when a converted AQL timestamp appears
in the future relative to a host sample. This is runtime conversion policy,
not a specified universal clock offset or an error bound. [Windows epoch
adjustment][windows-offset]

KFD samples GPU time, `CLOCK_MONOTONIC_RAW`, and `CLOCK_BOOTTIME`
sequentially. The host values use nanoseconds and the reported system
frequency is 1 GHz; the CPU sample is not an x86 TSC. GC9.4.3's driver reads
its GPU clock through GC instance 0 and notes partition variation. Neither
path supplies per-XCC calibration or a cross-partition offset bound. [KFD
sampling][kfd-clock] · [ROCr query][clock-query] · [GC9.4.3 reader][gc-clock]

Container width, effective hardware width, reset epoch, nominal rate, and
clock correlation remain distinct. Arithmetic across agents, partitions, or
XCC-local samples needs an established relationship for those domains. [GPU
observability](../observability.md) develops the conversion and host-bracket
uncertainty separately from packet capture.

## Queue metadata and alternate transports

ROCr gates metadata prefetch on an explicit request, KFD capability, and a
compiler target with major 12 and minor at least 5. Its metadata service is
separate from signal timestamp storage and the profiling-enable property.
[Metadata predicate][metadata] · [Queue property][queue-abi]

An embedded PM4 clock command and a separately scheduled PM4 queue also have
different dependency graphs. Publishing work to separate queues in host order
does not make one clock bracket the other's shader execution. The dependency
must cross those queues explicitly. The carrier route itself does not define
arbitrary counter commands or shader state for every architecture. [Carrier
implementations][pm4] · [AQL dependencies](barriers.md)

## Per-dispatch performance counters

ROCprofiler configures an agent and events, and AQLProfile generates vendor
packets with command/result storage. The ordinary injection order is START,
kernel, READ, then STOP. The interceptor adds ordering around instrumentation;
its serializer coordinates profiled kernels across managed queues. That
serialization changes the execution schedule and is not global exclusion
against other processes, raw queues, or unrelated profiling facilities.
[Packet construction][counter-packets] · [READ/STOP order][counter-order] ·
[Injection][intercept] · [Managed-queue serializer][serializer]

Event records carry coordinate identity. Agent geometry includes XCCs, shader
engines, and CUs; decoding preserves each event coordinate. Aggregation
requires the event definition, dimensions, enabled instances, counting
interval, and any multiplexing or replay policy. A 64-bit result container
does not define the physical counter width or units. [Agent/event
description][counter-agent] · [Coordinate decoding][counter-coordinates]

AQLProfile's packet-creation path allocates and copies generated storage
through caller callbacks. It does not submit work or acquire KFD PMC
ownership. Deletion releases that storage without waiting for GPU use or
releasing a native owner. The caller completes those independent lifetimes
first. [Construction and deletion][counter-lifetime]

### Application completion and profiling-tail completion

In the cited ordinary non-replay interception path, a barrier forwards the
application's original completion before the injected post-kernel
instrumentation. The profiling tail has its own signal and dependent barrier.
Acquiring the application signal therefore does not retire that tail.
[Completion ordering][counter-completion-order]

The record callback borrows a locally constructed record array. Records and
dispatch metadata needed afterward must be copied before callback return.
Processing normally uses a dedicated consumer, but runs synchronously on a
producer thread when that consumer is stopped or its ring is full. A
result-ready notification proves the copy, not return from the enclosing
handler. Conversely, a handler can enqueue record processing and finish while
the consumer still owns pending work. [Borrowed
records][counter-record-lifetime] · [Consumer and fallback][counter-consumer]
· [Handler retirement][counter-handler]

At this revision, stopping collection disables it and joins the dedicated
consumer, but callbacks for in-flight dispatches remain registered. Those
producers can still reach the synchronous fallback. Context stop alone is
therefore not producer quiescence. Queue-controller finalization separately
synchronizes active handlers; the queue sync can return failure with a
warning, and the controller discards that boolean result. Reaching the cleanup
call does not prove a successful drain. [Collection stop][counter-stop] ·
[Consumer join][counter-consumer] · [Queue sync][counter-sync] ·
[Finalization][counter-controller-fini]

Native queue reclamation is another boundary. At the cited revision, the
intercepted destroy path synchronizes and erases its SDK wrapper without
forwarding to the saved native queue destructor. The wrapper destructor
releases its active-work signal. A successful intercepted destroy return
therefore does not establish native queue allocation reclamation. [Destroy
interception][counter-destroy-entry] · [Wrapper
removal][counter-wrapper-removal] · [Wrapper
destructor][counter-wrapper-destructor]

These are properties of the linked source revision, not interchangeable
contracts for every SDK build. Command completion, borrowed callback data,
producer/consumer quiescence, native resource reclamation, and [counter
ownership](../observability.md#counter-ownership-and-interpretation) each
require their own completed transition.

Return to [AQL](README.md) or the [primary source map](../../sources.md).

[profile-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1140-L1329
[signal-abi]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_signal.h#L49-L78
[clock-packet]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/gfx9_cmd_builder.h#L450-L467
[clock-routing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/pm4/sqtt_builder.h#L454-L485
[pm4]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1762
[pal-timestamp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L397-L449
[queue-abi]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_queue.h#L49-L58
[api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L850-L925
[enable]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1587-L1606
[setter]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/queue.h#L446-L449
[clr-enable]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3577-L3638
[signal-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/signal.h#L188-L207
[translate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3196-L3329
[signal-stores]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/interrupt_signal.cpp#L128-L136
[pmc-admission]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L3392-L3415
[frequency]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L287-L302
[windows-offset]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3294-L3310
[kfd-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L701-L726
[clock-query]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2671-L2686
[gc-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L507-L519
[metadata]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L119-L126
[counter-packets]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/aql_packet.cpp#L106-L149
[counter-order]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/aql_packet.hpp#L157-L173
[intercept]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue.cpp#L815-L876
[serializer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/profile_serializer.cpp#L192-L238
[counter-agent]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/include/aqlprofile-sdk/aql_profile_v2.h#L174-L215
[counter-coordinates]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/include/aqlprofile-sdk/aql_profile_v2.h#L330-L341
[counter-lifetime]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/aqlprofile/src/core/counters.cpp#L235-L334
[counter-completion-order]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue.cpp#L870-L908
[counter-record-lifetime]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/sample_processing.cpp#L58-L157
[counter-consumer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/sample_consumer.hpp#L63-L109
[counter-handler]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue.cpp#L285-L359
[counter-stop]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/core.cpp#L210-L242
[counter-sync]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue.cpp#L1468-L1482
[counter-controller-fini]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue_controller.cpp#L888-L907
[counter-destroy-entry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue_controller.cpp#L151-L155
[counter-wrapper-removal]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue_controller.cpp#L493-L567
[counter-wrapper-destructor]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue.cpp#L1386-L1393
