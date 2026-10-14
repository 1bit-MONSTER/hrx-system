# GPU clocks and performance counters

A GPU observation has an execution point, a clock or event definition, a
visibility boundary, and an owner. A timestamp store alone does not identify
shader duration or correlate device time with a host clock. Counter collection
additionally owns selectors, hardware instances, and enable/reset/read/stop
state.

## Observation surfaces

| Surface | Meaning |
| --- | --- |
| PM4 command-processor timestamp | COPY_DATA from the GPU-clock source samples CP progress; earlier asynchronous shader or DMA work needs a separate dependency. |
| PM4 shader-end timestamp | A bottom-of-pipe RELEASE_MEM can sample a shader-completion stage. Timestamp-store visibility and lifetime still need the surrounding event/wait/cache sequence. |
| SDMA global timestamp | GET_GLOBAL writes a raw timestamp. PAL orders preceding transfer work before its timestamp operation; host visibility and command storage lifetime remain separate obligations. |
| AQL-carried CP timestamp | A native vendor IB executes a clock copy, with explicit virtual-XCC selection where needed and a native carrier completion. |
| AQL dispatch profiling | A profiling-enabled queue captures start/end in the dispatch's native completion signal. That storage remains owned until the result is read. |
| [Firmware dispatch logs](profiling/dispatch-log.md) | A vendor KFD service exposes per-process compute dispatch records in native overwrite rings, with source-specific clock, identity and collection rules. |
| Shader cycle counter | A wave samples core cycles through the target's scalar-memory, shader-register or dedicated counter instruction. Width and SIMD epoch determine which samples can be compared. |
| Shader realtime counter | A wave samples a fixed-frequency clock through S_MEMREALTIME or a returned REALTIME message. The result's readiness, frequency and correlation remain separate contracts. |
| Programmable counters | A collection observes a specified event across selected hardware instances and a defined counting interval. |
| [Program-counter sampling](profiling/pc-sampling.md) | Native triggers and a trap writer capture wave state; per-XCC buffer rotation publishes completed samples while the workload continues. |
| [Shader thread traces](profiling/thread-trace.md) | SQTT token streams use source-specific finite drain or live buffer replacement, with explicit loss and decoding context. |
| [Pipeline statistics](pm4/pipeline-statistics.md) | Beginning and ending counter samples, including CS invocations, with separate result records, availability and final-reader ownership. |
| Host timing | A host clock measures publication, submission through observed completion, or another explicitly bounded host interval. |

The command and lifetime details belong to [PM4](pm4/), [SDMA](sdma/), and
[AQL profiling](aql/profiling.md). The execution points in these interfaces
are not interchangeable clock calibration evidence.

## Clock domains and conversion

A timestamp description identifies the physical device and partition, clock
domain, effective width, units, nominal frequency, epoch/reset behavior, and
sampled execution stage. Equal integer widths or a nondecreasing sequence do
not establish a relationship between shader cycles, engine clocks, global GPU
time, and host monotonic time.

The sources expose a concrete width distinction. RADV advertises 64 valid
timestamp bits, while the SMUIO 13.0.6 GOLDEN_TSC registers used by Linux's
GFX11 physical-function reader have 24 upper and 32 lower count bits. The
reader selects another clock path for a virtual function. These facts alone
establish neither a 64-bit hardware wrap period nor that every PM4/SDMA
timestamp is a modulo-56-bit view of GOLDEN_TSC. Exact IP and clock routing
remain necessary to connect those representations. [GFX11 reader][gfx11-clock]
· [Register fields][golden-width] · [RADV properties][radv-width]

KFD's `GET_CLOCK_COUNTERS` reads GPU time, then host `CLOCK_MONOTONIC_RAW`,
then `CLOCK_BOOTTIME`. The host samples are nanoseconds; the reported 1 GHz
system frequency belongs to them, not the GPU. They are sequential samples
without a returned simultaneous-sampling error bound. The CPU field is not an
x86 TSC. [KFD sampling][kfd-clocks]

The nominal GPU rate comes from a separate native query. Linux's SOC21 path
corrects the nominal 100 MHz APU reference to 99.81 MHz for its stated case;
PAL converts the driver's GPU-counter frequency from kHz to Hz. Shader and
memory operating clocks are different quantities. [SOC21 reference
clock][soc21-clock] · [PAL frequency][pal-frequency]

| DRM query | Returned observation |
| --- | --- |
| `AMDGPU_INFO_TIMESTAMP` | A 64-bit value from the selected device's GFX clock reader. |
| `AMDGPU_INFO_DEV_INFO` | `gpu_counter_freq`, the nominal GPU counter frequency in kHz, separately from shader/memory operating clocks. |

These requests use `DRM_IOCTL_AMDGPU_INFO` on the selected device descriptor.
[Timestamp query][drm-clock] · [Device information][drm-frequency]

PAL's `GetCalibratedTimestamps` brackets the GPU query with host
`CLOCK_MONOTONIC_RAW` and `CLOCK_MONOTONIC` reads. It returns the earlier host
values and the larger bracket width as `maxDeviation`. Its GPU sample is
documented as compatible with `CmdWriteTimestamp`. The implementation does not
take an atomic host/GPU snapshot. [PAL bracketing][pal-calibrated] · [Sample
contract][pal-calibrated-type]

Windows `D3DKMTQueryClockCalibration` identifies the adapter, physical adapter
and engine node, and returns GPU frequency plus GPU/CPU counter samples. Its
`ContextManagementProcessor` flag distinguishes a context-management clock
from the underlying engine clock, which may differ. The selected native queue
scheduler determines the engine node; node zero is not a portable substitute.
[Query identity][kmt-clock] · [Clock selection][kmt-clock-flags]

Native-query compatibility needs evidence at the packet source. In a bounded
Linux GFX11.0.0 experiment, a confirmed PM4 clock pair was
`80395912441212..80395912441500`, while the surrounding DRM samples were
`80395911298728..80395911507976`. The packet values exceeded even the later
native sample despite normal copy completion and retirement. At the reported
100 MHz rate, the discrepancy is millisecond scale. The result falsifies a
universal shared-epoch assumption; it does not identify the source of the
offset or establish a portable correction. Actual queue-written anchors avoid
that assumption. A reported nominal rate is likewise not an oscillator-error
bound against the host clock.

### Host brackets and uncertainty

For one chosen host domain, retain `(host_before_ns, gpu_ticks,
host_after_ns)`. The GPU query's sampling point lies within the host bracket;
the bracket width expresses sampling uncertainty, not timestamp-store
atomicity or physical frequency accuracy. Its midpoint is an estimate, not an
exact correspondence. This interpretation follows the ordering of PAL's
bracketing reads. [Sampling implementation][pal-calibrated]

For ordered tuples `Q0 = (a0, g0, b0)` and `Q1 = (a1, g1, b1)`, the host
elapsed interval between the GPU sampling points is bounded by `[a1 - b0, b1 -
a0]`. Within one non-wrapping clock epoch, a nominal-rate model predicts `(g1
- g0) * 1,000,000 / frequency_kHz` nanoseconds. Subtracting ticks before
wide-integer or rational conversion preserves precision. Clock-read
quantization, any physical rate-error bound, and reset continuity are
additional facts, not consequences of this interval arithmetic.

Agreement checks that nominal-rate model over the observed interval. It does
not establish zero clock offset or calibrate every engine. Bracketing a whole
command sequence can establish coarse epoch compatibility if all retained
packet samples lie inside the outer GPU range and have the same source domain;
it does not locate the individual command samples precisely. Packet execution,
visibility, and storage lifetime remain prerequisites to interpreting those
values. [Native sample source][drm-clock] · [Sample
compatibility][pal-calibrated-type]

ROCr obtains wallclock frequency separately and converts signal timestamps
through paired GPU/system samples. It handles drift and bounded extrapolation,
translating the interval's end first to reduce clock-measurement jitter. Each
scalar conversion locks the shared clock state separately; the pair has no
common calibration snapshot. GC9.4.3's native reader notes partition
variation and reads through GC instance 0. Neither a compiler target nor one
physical package establishes per-XCC or cross-partition calibration. [ROCr
frequency][rocr-frequency] · [Translation][rocr-clocks] · [Partition-aware
reader][partition-clock]

### Shader cycle counters

Clang lowers `__builtin_readcyclecounter()` and
`__builtin_readsteadycounter()` to separate LLVM intrinsics. Their AMDGPU
patterns select different clock sources. The following table describes the
cited compiler's cycle-counter representations and feature predicates;
instruction availability alone does not select a pattern when predicates
overlap. [Frontend][counter-builtins] [Cycle and steady patterns][counter-patterns]
[Target features][counter-features]

| Cycle-counter mechanism | Predicate and target evidence | Returned representation |
| --- | --- | --- |
| `S_MEMTIME` | `HasSMemTimeInst`; SI, CI, VI, GFX9 and GFX10 base feature sets include it. | 64-bit scalar-memory result. The GCN3 instruction description identifies shader core clocks. |
| `S_GETREG_B32 HW_REG_SHADER_CYCLES` | `HasShaderCyclesRegister`; GFX11 common features include it, including GFX11.5 and GFX11.7 inheritance. | Bits 0–19 of the counter, with an explicitly zero high DWORD in the 64-bit LLVM result. |
| `GET_SHADERCYCLESHILO` | `HasShaderCyclesHiLoRegisters`; the ordinary GFX12 feature set includes it. | Three 32-bit reads of HI, LO, HI, assembled into a 64-bit result as described below. |
| `S_GET_SHADER_CYCLES_U64` | `HasSGetShaderCyclesInst`, implemented as `HasGFX1250Insts`; the GFX12.50 feature set includes it. | One 64-bit shader-cycle read. |

[Older features][counter-old-features] [GFX11 features][counter-gfx11-features]
[GFX12 features][counter-gfx12-features] [GFX12.50 features][counter-gfx1250-features]
[Register width encoding][counter-hwreg] [HI/LO pseudo][counter-hilo-pseudo]
[Dedicated instruction][counter-direct] [Dedicated predicate][counter-direct-predicate]
[Subtarget method][counter-direct-method]
[GCN3 instruction, p. 12-56][gcn3-cycle]

The compiler's expected-output fixture selects `S_MEMTIME` for its GFX10.3
case even though that target also advertises the shader-cycle register. It
selects the direct 64-bit instruction for GFX12.50 despite that target also
advertising HI/LO registers. Those expectations cover both SelectionDAG and
GlobalISel. A feature list alone cannot resolve these overlaps. GFX13 also
declares `FeatureGFX1250Insts` alongside `FeatureShaderCyclesRegister`; the
listed fixture contains no GFX13 case. [Expected selections][counter-fixture]
[GFX10 inheritance][counter-gfx10-features] [GFX10.3 overlap][counter-gfx103-features]
[GFX13 declarations][counter-gfx13-features]

RDNA3 and RDNA3.5 describe `SHADER_CYCLES` as a 20-bit counter whose epochs
are not synchronized between SIMDs. Their stated use is a time delta within
one wave. For an interval shorter than one counter period in that domain,
raw subtraction modulo `2^20` handles a single wrap; zero-extending each
sample to 64 bits does not change the modulus. Separate dispatches do not
inherit one wave's counter epoch. [RDNA3 §3.4.10][rdna3-time]
[RDNA3.5 §3.4.10][rdna35-time]

RDNA4 and CDNA5 widen the shader counter to 64 bits but retain the same
within-wave restriction. RDNA4 recommends reading HI, LO, HI and repeating
the reads if the high halves differ. The cited compiler's HI/LO expansion
instead returns `HI2:LO1` when the highs agree and `HI2:0` otherwise; its
stated intent is a value attained during the three-read interval. This
expansion performs no retry. Neither algorithm establishes a common epoch
between SIMDs. [RDNA4 §3.4.11][rdna4-time] [CDNA5 §3.4.11][cdna5-time]
[Compiler expansion][counter-hilo-expand]

### Shader realtime and frequency

The steady-counter pattern uses `S_MEMREALTIME` under `HasSMemRealTime`,
present in VI, GFX9 and GFX10 base features. For its `isGFX11Plus` predicate,
the compiler uses `S_SENDMSG_RTN_B64` with `MSG_RTN_GET_REALTIME` (`0x83`).
The RDNA3 manual pairs that instruction with `S_WAITCNT LGKMcnt == 0`;
RDNA4 uses `S_WAIT_KMcnt == 0`. These waits make the returned register value
ready; they do not join arbitrary shader work or publish a result to another
actor. [Patterns][counter-patterns] [Older features][counter-old-features]
[Generation predicate][counter-steady-predicate] [RDNA3 §3.4.10][rdna3-time]
[RDNA4 §3.4.11.1][rdna4-time]

GCN3 defines `S_MEMREALTIME` as independent of core-frequency and power-mode
changes. The RDNA3, RDNA3.5 and RDNA4 manuals identify realtime as the
clock-generator source for comparisons between waves or SIMDs, including
idle intervals. Their typical 100 MHz rate is not a device-specific frequency
query or a host-clock correlation. Core-cycle counts, this fixed-frequency
source and native packet timestamps retain their own conversion inputs.
[GCN3 §7.2.6][gcn3-realtime] [RDNA3 time][rdna3-time]
[RDNA3.5 time][rdna35-time] [RDNA4 time][rdna4-time]

`HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY` reports the agent's maximum clock
in MHz. It does not report an observed shader rate or the realtime frequency.
The [OpenCL child scheduler](aql/device-enqueue.md#events-clocks-and-final-users)
uses that maximum to scale cycle-counter samples from separate scheduler
passes. Frequency scaling alone cannot repair counter rollover or unrelated
SIMD epochs. [HSA property][shader-max-frequency]

## Counter ownership and interpretation

The cited mainline KFD UAPI introduces profiler control at interface version
1.23 and gives that service a separate profiler protocol version. Vendor
drivers can backport it under another ioctl namespace at an older KFD version.
The SDK selects the mainline request at 1.23 or later and the vendor request
otherwise, then queries the protocol. That comparison selects a request; it
does not establish availability or permission. Absence of this owner interface
does not imply absence of hardware counters or queue timestamp capture.
[UAPI history][kfd-profiler-uapi] · [Profiler protocol][kfd-profiler-protocol] ·
[SDK request selection][sdk-profiler-request]

Where implemented, KFD's PMC operation arbitrates a profiler owner. It does
not suspend all competing workloads, attribute counters to one queue, or
establish exclusion against every other profiling facility.

| Boundary | Native KFD contract at the cited revision |
| --- | --- |
| Permission | Acquire and release require `perfmon_capable()`: `CAP_PERFMON` or `CAP_SYS_ADMIN` in the initial user namespace. Device-node access alone is insufficient. |
| Identity | A `kfd_process` owns the device, without a nesting count. Reacquisition by that owner returns `EALREADY`; a competitor receives `EBUSY`. Release checks ownership. |
| Device and node | The owner pointer resides on `kfd_dev`. Existing-queue perfcount changes traverse the selected node's device queue manager and its process queue lists, not every logical node. |
| Work completion | Owner acquire/release neither completes collection packets nor publishes their results. |
| Process cleanup | Process-device teardown requests perfcount disable and clears the owner. Closing a primary KFD descriptor only drops a process reference; it is not an immediate unlock receipt. |

[Owner operations][kfd-profiler] · [Permission predicate][perfmon] ·
[Permission namespace][capable] · [Owner field][kfd-owner] · [Queue-manager
scope][kfd-perfcount] · [Process cleanup][kfd-profiler-release] · [Descriptor
close][kfd-close]

Owner acquisition/release also changes a separate PTL-disable request on paths
where that mechanism applies. The owner pointer changes before the fallible
action. An error can therefore follow partial progress: failed acquisition
does not prove unchanged state, and failed release does not prove that
ownership remains held. This is not a transactional constructor/destructor
pair. [Native state changes][kfd-profiler]

Queue creation and update differ. GFX9/GFX11 compute MQD initialization
enables perfcount when the parent has an owner; their update paths also handle
explicit enable/disable flags. The cited GFX12 initializer checks the owner
but its updater lacks those flag handlers. An existing-queue disable request
therefore does not define every later queue's state or every family's update
behavior. [GFX9][kfd-mqd9] · [GFX11][kfd-mqd11] · [GFX12][kfd-mqd12]

The cited existing-queue path also contains conflicting lock ownership across
its nested update and discards queue-update errors. These source-level facts
prevent interface availability from proving a completed ownership transition.
They do not establish the behavior of another driver build. A native owner
contract must include the actual update and cleanup implementation, not only
its UAPI number. [Outer update][kfd-perfcount] · [Queue
update][kfd-queue-update] · [Selected callback][kfd-update-callback] · [Mutex
ownership][kfd-dqm-lock]

ROCprofiler's device-counting path attempts ownership before starting counters
and waits for stop before release, but continues after ownership failures. Its
helper named `counter_collection_has_device_lock` probes interface
availability. Neither that probe nor continuation establishes exclusive
collection. AQLProfile packet construction itself acquires no owner; its
command/result allocations have a separate [completed-use
boundary](aql/profiling.md#per-dispatch-performance-counters). [SDK owner
helper][sdk-counter-owner] · [Start][sdk-counter-start] ·
[Stop][sdk-counter-stop]

### Other state owners

ROCr's queue profiling bit enables dispatch timestamp capture without
acquiring the PMC owner. PAL changes clock mode through DRM stable pstate,
with a sysfs alternate path. DRM's `stable_pstate_ctx` is a separate owner
from KFD's profiler process. These mechanisms do not individually establish
cross-tool counter exclusion. [Dispatch
capture](aql/profiling.md#dispatch-timestamps) · [PAL clock
mode][pal-clock-mode] · [DRM clock owner][drm-clock-owner]

PAL's GFX11 experiment opens a counter programming window, waits for prior
work, resets and configures selectors, starts counting, then samples and
disables collection. It stops global counters for sampling; the GFX10
SQ-counter workaround instead leaves them running for the ending sample. This path
explicitly lacks per-context filtering. GFX11 counter-clock control belongs to
the kernel driver. Register definitions alone therefore do not make counters
queue-local or grant access on an arbitrary native queue. [PAL experiment
lifecycle][pal-counters] · [Workaround selection][pal-never-stop] · [Sampling
states][pal-sample]

The [PM4 counter chapter](pm4/counters.md) supplies the GFX11 field and instance
layouts and follows PAL's cumulative-sample owner through result decoding and
submission retirement.

[RADV performance queries](pm4/counter-queries.md) separate a host profiling
reference from a GPU mutex in a private per-device allocation. The host
reference controls stable pstate; the mutex serializes participating query
submissions. Every exposed counter is marked as affected by concurrent work.
Neither owner establishes exclusion against all other collectors.

[Streaming performance monitoring](profiling/spm.md) separates hardware ring
layouts from the vendor KFD transport's asynchronous host-buffer rotation.
Repeated snapshots, overwrite capture and native buffer rotation have different
publication and storage-retirement boundaries.

SDMA counters participate in that CP-managed lifecycle. PAL's GFX11 DMA block
is global, with two counter modules per available instance and at most two
instances; Mesa describes the same inventory. PAL selects instance-specific
registers and copies result halves after the selected sampling sequence. A DMA
instance is not a queue ordinal. A CP idle boundary alone cannot bracket work
on a separate SDMA queue: dependencies are required before and after that
work. SDMA GET_GLOBAL does not acquire or collect these counters. [PAL DMA
inventory][pal-dma] · [Mesa inventory][mesa-dma] · [PAL instance
selection][pal-counter-select] · [PAL readback][pal-counter-readback] ·
[Result halves][pal-counter-halves]

Power policy can determine whether counters function. AMD's profiling
documentation identifies gated perfmon clocks on some RDNA3/RDNA4 blocks in
AUTO mode and driver-controlled STABLE_STD enablement. A zero from an
unavailable observation surface is not evidence that the event did not occur.
This prerequisite is distinct from choosing a reproducible operating
frequency. [AMD counter-clock requirement][pmc-clocks]

### DRM stable-pstate lifetime

`AMDGPU_CTX_OP_SET_STABLE_PSTATE` operates through a DRM context, while Linux
stores the current owner in the device's `stable_pstate_ctx`. A different
context receives `EBUSY` while that owner exists. At the pinned revision,
the setter returns without creating an owner when the requested policy already
matches the current policy. Otherwise a successful policy change establishes
the first owner and saves the preceding policy in that context.
[Native setter][drm-clock-setter]

`AMDGPU_CTX_STABLE_PSTATE_NONE` selects automatic policy; the setter does not
clear ownership when processing it. When `drm_dev_enter` admits access to the
live device, context finalization attempts to restore the saved policy, then
clears `stable_pstate_ctx` without propagating the restoration result. If
device removal prevents entry, that restoration and owner-clearing block does
not execute. Policy selection, ownership release and successful policy
restoration are distinct transitions in this implementation.
[Context finalization][drm-clock-finalize]

RADV's first host profiling reference requests its configured pstate; its
last release requests `NONE` and discards the result. The winsys retries
`EBUSY` within the supplied interval, and the release path supplies 100 ms.
Public profiling-lock release therefore does not establish that the DRM
context owner has gone away or that a clock request succeeded. RADV device
destruction forwards context-free requests. Linux removes the context handle
and drops its reference; the last reference invokes native finalization.
[RADV host references][radv-clock-owner]
[Winsys request][radv-clock-request] [Device teardown][radv-device-destroy]
[Context teardown][radv-context-destroy] [DRM forwarding][radv-context-ioctl]
[Handle removal][drm-context-free] [Final-reference release][drm-context-put]

### Event identity and aggregation

| Property | Interpretation requirement |
| --- | --- |
| Event and units | Architecture, block, selector, and raw event definition; a derived formula retains its constituent events. |
| Counting interval | Start/reset/read/stop ordering, result visibility, and completed use of selectors and result storage. |
| Width | Effective bits, wrap/saturation, read-latch protocol, and accumulation width. |
| Attribution | Device/context/process/queue scope and interference from participants outside the collection owner. |
| Dimensions | XCC, shader-engine, CU/WGP, or other instance coordinates until the defined sum, average, or normalization. |
| Multiplexing | Compatible groups per pass and a replayable workload when separate passes are required. |
| Perturbation | Instrumentation, waits, serialization, cache maintenance, and any policy changes included in the interval. |

Dispatch counting and device counting answer different questions. Event
dimensions depend on both event and architecture/agent geometry. The SDK's
version-1 counter-info query selects a representative agent internally rather
than accepting an agent argument; collected records carry coordinates for the
actual values. A dimension absent from an event is not an additional physical
instance to aggregate. [Collection modes and dimensions][counter-services]

## Defining a measured interval

| Interval | Included work and completion boundary |
| --- | --- |
| Host publication | Reservation, capacity handling, body/header publication, and doorbell operations selected by the caller. Final execution completion lies outside this interval. |
| End to end | Publication through the chosen host wait observing completion; the wait policy is part of the interval. |
| Device execution | Device samples with explicit dependencies around the named shader, transfer, or handoff stages. |
| Transfer rate | Defined payload bytes, placement, direction, concurrency, and final completion. Payload bandwidth is not bus traffic. |
| Cross-queue handoff | Producer release, dependency, consumer acquire, and terminal result, with the complete graph stated. |

Host publication return does not identify the instant at which a posted
doorbell store reaches the device. The [AQL publication](aql/) and
[barrier](aql/barriers.md) protocols retain their own execution and storage
boundaries outside a shorter host interval.

A host clock bracket has observation cost. A matched empty bracket can expose
that cost under the same clock and compiler-ordering conditions; subtracting
median costs does not prove device latency. Batching changes the submission
graph and in-flight ownership. Reusing resident allocations avoids repeated
allocation, but does not establish sustained throughput or continuous cache
warmth.

Likewise, replacing two host-coordinated shader launches with a GPU dependency
changes reservations, packet count, fences, signal operations, and host waits.
The resulting completed-pair interval measures that whole submission strategy.
It is not an isolated cost of one barrier bit. Both forms retain intermediate
payload and executable storage until their final consumer completes.
[Dependency and owner graph](aql/barriers.md#fan-in-and-independent-worksets)

Return to the [GPU index](README.md) or [primary source map](../sources.md).

[gfx11-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L5301-L5342
[golden-width]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/smuio/smuio_13_0_6_sh_mask.h#L50-L55
[radv-width]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L3008-3050
[kfd-clocks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L701-L726
[soc21-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/soc21.c#L258-L267
[pal-frequency]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L960-L974
[drm-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L777-L779
[drm-frequency]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L932-L951
[pal-calibrated]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L1957-L1994
[pal-calibrated-type]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palDevice.h#L2503-L2512
[rocr-frequency]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L287-L302
[rocr-clocks]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3196-L3329
[partition-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L507-L519
[kfd-profiler-uapi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L50-L58
[kfd-profiler-protocol]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L1560-L1588
[sdk-profiler-request]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/ioctl.cpp#L38-L99
[kfd-profiler]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L3333-L3432
[perfmon]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/linux/capability.h#L195-L198
[capable]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/kernel/capability.c#L403-L417
[kfd-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L325-L392
[kfd-perfcount]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L331-L352
[kfd-profiler-release]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L1177-L1202
[kfd-close]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L145-L195
[kfd-mqd9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L234-L325
[kfd-mqd11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L183-L269
[kfd-mqd12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L158-L245
[kfd-queue-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L648-L687
[kfd-update-callback]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1070-L1079
[kfd-dqm-lock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.h#L352-L365
[sdk-counter-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/ioctl.cpp#L85-L166
[sdk-counter-start]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/device_counting.cpp#L417-L560
[sdk-counter-stop]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/device_counting.cpp#L571-L652
[pal-clock-mode]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L5130-L5245
[drm-clock-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.c#L341-L435
[drm-clock-setter]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.c#L341-L405
[drm-clock-finalize]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.c#L408-L440
[drm-context-free]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.c#L510-L518
[drm-context-put]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.h#L72-L78
[radv-clock-owner]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1952-L2041
[radv-clock-request]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1623-L1651
[radv-device-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1406-L1423
[radv-context-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1557-L1572
[radv-context-ioctl]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_linux_drm.c#L330-L370
[pal-counters]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2037-L2310
[pal-dma]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfCtrInfo.cpp#L1672-L1694
[mesa-dma]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_perfcounter_gfx11.c#L817-839
[pal-never-stop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L1840-L1843
[pal-sample]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3687-L3730
[pal-counter-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3465-L3503
[pal-counter-readback]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3865-L3896
[pal-counter-halves]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3948-L3964
[pmc-clocks]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/docs/how-to/using-rocprofv3.rst#L31-L39
[counter-services]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/docs/api-reference/counter_collection_services.rst#L10-L82
[counter-builtins]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/clang/lib/CodeGen/CGBuiltin.cpp#L4135-L4142
[counter-patterns]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/SMInstructions.td#L1096-L1124
[counter-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L1262-L1274
[counter-old-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L1715-L1845
[counter-gfx11-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L2303-L2417
[counter-gfx12-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L2419-L2488
[counter-gfx1250-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L2490-L2581
[counter-hwreg]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/SIInstrInfo.td#L1835-L1859
[counter-hilo-pseudo]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/SIInstructions.td#L466-L472
[counter-direct]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/SOPInstructions.td#L1771-L1775
[counter-direct-predicate]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L3263-L3265
[counter-direct-method]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/GCNSubtarget.h#L748-L749
[counter-fixture]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/test/CodeGen/AMDGPU/readcyclecounter.ll#L1-L72
[counter-gfx10-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L2216-L2225
[counter-gfx103-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L2260-L2299
[counter-gfx13-features]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L2720-L2736
[counter-hilo-expand]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/SIISelLowering.cpp#L7267-L7304
[counter-steady-predicate]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPU.td#L3095-L3097
[shader-max-frequency]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L732-L736
[gcn3-cycle]: https://gpuopen.com/download/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf#page=162
[gcn3-realtime]: https://gpuopen.com/download/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf#page=65
[rdna3-time]: https://gpuopen.com/download/rdna3-shader-instruction-set-architecture-feb-2023.pdf#page=36
[rdna35-time]: https://gpuopen.com/download/rdna35_instruction_set_architecture.pdf#page=36
[rdna4-time]: https://gpuopen.com/download/rdna4-instruction-set-architecture.pdf#page=40
[cdna5-time]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=37

[kmt-clock]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_d3dkmt_queryclockcalibration
[kmt-clock-flags]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_gpuclockdata_flags
