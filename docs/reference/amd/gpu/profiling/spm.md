# Streaming performance monitoring

Streaming performance monitoring (SPM) periodically samples selected hardware
counters into memory through the RLC. Counter selection, sample layout, ring
transport and host-buffer retirement have separate contracts. A ring write
pointer identifies producer progress; it does not by itself protect a sample
from overwrite while a consumer reads it.

## Capture modes

| Path | Storage and consumption |
| --- | --- |
| PAL performance experiment | Caller-provided GPU ring. The completed-trace reader reconstructs the retained sample order after collection. |
| Vendor KFD SPM service | Driver-owned GPU rings plus asynchronously borrowed host destinations. Native destination rotation returns the preceding destinations to the caller. |

These are different transports over the SPM producer. PAL's layout and completed
trace parser do not establish a live reader's publication protocol. The vendor
KFD service supplies an additional kernel copy and ownership boundary; its UAPI
is not present in the cited mainline KFD header. [PAL layout][pal-layout]
[PAL reader][pal-reader] [Vendor service][spm] [Mainline UAPI][mainline-uapi]

## Sample representation

PAL's GFX9-family implementation describes a ring-relative write-pointer offset,
write-pointer granularity, first-sample offset, sample stride, capacity and
per-counter field offsets. Samples start after the first 32-byte line. Its
GFX11 path expresses the write pointer in 32-byte lines; the other path in that
function uses bytes. Counter values can occupy one 16-bit field or two separately
located 16-bit halves forming a 32-bit value. The result retains block, event
and instance identity. Neither every counter nor every architecture has the same
field width. [Layout construction][pal-layout] [Field decoding][pal-reader]

A completed ring can contain only a retained suffix. Reconstructing that suffix
does not provide a count of samples overwritten during capture or a consumer
credit protocol. Sample timestamps, hardware result values and a queue's
completion marker retain their separate meanings.

## Sampling interval and clock selection

The ROCprofiler SDK's embedded AQLProfile distinguishes shader-clock intervals
from reference-clock intervals. Its public caller admits reference-clock mode
on exactly `gfx1200`, `gfx1201`, and `gfx1250`; it rejects that parameter on
other agents before building commands and removes it from their advertised
configuration list. The lower-level configuration enumerator alone is broader:
it lists reference-clock mode for factory IDs above `GFX10_GPU_ID`, while its
command-construction check rejects that mode below `GFX12_GPU_ID`. The caller's
predicate is therefore part of the usable contract. Factory enumeration order
is not a hardware capability test. [Caller admission][interval-admission]
[Parameter rejection][interval-validation]
[Advertised configurations][interval-filter] [Factory IDs][factory-ids]
[Lower-level enumeration][interval-enumeration] [Build check][interval-build]

For the GFX12 primitive, `RLC_SPM_PERFMON_CNTL` contains:

| Bits | Field | Value emitted by the builder |
| --- | --- | --- |
| 13:12 | `PERFMON_RING_MODE` | 3, identified by the builder as stall and interrupt. |
| 14 | `PERFMON_SAMPLE_INTERVAL_START_MODE` | 0. |
| 15 | `PERFMON_SAMPLE_INTERVAL_TYPE` | 0 for SCLK, 1 for REFCLK. |
| 31:16 | `PERFMON_SAMPLE_INTERVAL` | Requested interval minus one, representing 1 through 65,536 selected-clock cycles. |

[GFX12 field definitions][interval-fields12]
[GFX12 encoding][interval-encode12]

The older GFX9/GFX10 factory path reports SCLK intervals from 32 through
65,504 cycles in multiples of 32; its normalization rounds a request to the
nearest multiple using `(N + 16) & ~31`. The GFX11 primitive writes its interval
directly instead of using the GFX12 count-minus-one encoding. Transferring an
interval field across these builders without its encoding changes its meaning.
[Interval normalization][interval-normalize] [GFX11 encoding][interval-encode11]

Selecting an interval clock does not correlate the resulting records with the
host or with another GPU engine. An SCLK interval also needs the operating
frequency over that interval to express elapsed time. The reference-clock
selection and an independently obtained clock rate avoid that particular
shader-frequency dependency; neither supplies a cross-clock epoch by itself.
[Clock domains](../observability.md#clock-domains-and-conversion)

## Vendor KFD destination ABI

The vendor request `AMDKFD_IOC_SPM` uses ioctl number `0x84` and the following
32-byte `kfd_ioctl_spm_args`. Operations are `ACQUIRE = 0`, `RELEASE = 1` and
`SET_DEST_BUF = 2`. [Vendor UAPI][spm-uapi]

| Byte offset | Field | Meaning |
| --- | --- | --- |
| 0 | `uint64_t dest_buf` | New host virtual address; zero unsets the destination. |
| 8 | `uint32_t buf_size` | Total destination capacity in bytes. |
| 12 | `uint32_t op` | Native operation. |
| 16 | `uint32_t timeout` | Fill-wait interval in milliseconds, updated by the wait path. |
| 20 | `uint32_t gpu_id` | KFD node identity. |
| 24 | `uint32_t bytes_copied` | Sum of payload bytes returned from preceding sub-buffers. |
| 28 | `uint32_t has_data_loss` | Number of preceding sub-buffers reporting possible loss. |

The driver divides the supplied capacity by its active XCC count and rounds each
sub-buffer capacity down to a multiple of 32 bytes. Each sub-buffer begins with
a 32-byte `kfd_ioctl_spm_buffer_header`: version, payload byte count, loss flag,
and five reserved DWORDs. Version bits 31:24 hold the major number and 23:0 the
minor number; the cited implementation writes version 0.4. Headers are filled
when the old destination is retired. The aggregate payload count is not one
contiguous byte extent: each XCC has a separate header, capacity and valid
length. [Partitioning][spm-rotate] [Header publication][spm-header]

The capacity check also runs for a zero destination. Each computed sub-buffer
must exceed its 32-byte header even when the operation unsets the destination.
[Partitioning and validation][spm-rotate]

## Execution and buffer lifetime

Acquisition allocates a 4 MiB kernel buffer per XCC, subtracting the overflow
reserve and ring metadata from usable capacity. A native monitor schedules a
copy worker, which enters the process address space and copies ring data into
the retained host virtual range. Returning from the initial destination call
therefore does not end the driver's access to that range. The caller preserves
its mappings and storage until successful rotation or native release joins the
writers. [Allocation][spm-acquire] [Monitor][spm-monitor] [Copy worker][spm-copy]

With an old destination attached, a zero-timeout rotation schedules and joins
the copy worker before retiring that destination. A nonzero timeout waits for
the driver's fill condition, which becomes true when any XCC sub-buffer fills.
It does not require every sub-buffer to fill. Ordinary timeout expiry returns
success and retires the bytes already copied. Although the UAPI prose promises
a partial-data copy on timeout, the cited implementation only explicitly
schedules that copy for zero timeout or its `-ETIME` path; ordinary wait expiry
returns zero and bypasses that branch. [Wait and rotation][spm-rotate]
[Fill condition][spm-copy] [UAPI description][spm-uapi]

Rotation removes the old destination under the copy-worker mutex, writes its
header, then installs the new destination. Successful return permits reuse of
the old host range while the new range remains borrowed. This supplies a
bounded two-buffer streaming flow:

1. Acquire the native SPM transport and configure the selected counter stream.
2. Attach host range A and start the admitted producer sequence.
3. Rotate to B; consume the returned per-XCC extents in A.
4. After consumption, rotate back to A and consume B. Repeat independently of
   the workload's total duration.
5. Complete the producer's stop sequence before collecting its final published
   data. Rotate with zero destination and an admitted capacity, then release
   the native transport.

The final step needs the producer-specific stop and write-completion boundary.
Joining the kernel copy worker alone does not prove that the GPU cannot publish
another sample. Native release stops SPM, joins the monitor and copy work, and
frees the kernel rings; it does not publish a final user header or return a final
valid extent. Releasing without prior collection can discard the tail.
[Rotation][spm-rotate] [Stop and release][spm-release]

## Device-scoped producer and collector sequence

The SDK's device-scoped SPM caller starts the native transport and collector
before submitting the producer's start packet. It then waits with acquire
semantics for that packet's completion. This ordering lets the native driver
initialize its ring and read pointer before PM4 enables sampling. The builder
intentionally leaves the SPM reset to KFD: resetting the write pointer without
the kernel-owned read pointer would break their common ring position.
[Device start][device-start] [Native collector initialization][collector-start]
[Reset ownership][builder-start]

Stopping reverses those producer/collector dependencies. The caller submits
the producer's stop packet, waits for its completion, then stops the host
collector. The collector joins its worker threads before releasing native SPM.
This separates command completion, destination retirement, and native ring
release; they are not one interchangeable stop operation.
[Device stop][device-stop] [Collector bridge][collector-bridge]
[Worker and native teardown][collector-start]

For factory IDs at or above `GFX11_GPU_ID`, the stop builder first requests a
one-SCLK-cycle sampling interval to materialize the partial final interval.
That changes both the interval and its source even when capture used REFCLK.
It then emits the idle dependency and stops SPM without resetting its ring.
The final sample therefore does not represent another ordinary configured
sampling interval. The command sequence does not expose a separate
acknowledgement of that requested final sample.
[Stop selection][interval-build] [Stop commands][builder-stop]

The collector continues destination rotation after the stop request, but its
loop can exit after six nonempty returned extents instead of observing an
empty drain. That is a consumer policy, not evidence that every final GPU
write reached a returned destination. A final-data guarantee needs the
producer's completed publication and the native ring's drained extent; a fixed
number of rotations cannot establish either condition.
[Drain exit conditions][collector-drain]

The caller also requests the native PMC owner and requests PTL disablement.
Its version query and ownership request are separate calls, and this caller
does not gate start on their returned statuses. Successful SDK start therefore
does not by itself prove exclusive ownership or successful native-control
configuration. [Caller controls][device-controls]
[Native control results][native-controls]

## Errors, loss and ownership

Rotation is not transactional across XCCs. A header copy can fail after the old
destination was removed. A later XCC can fail after earlier XCCs accepted the
new destination. An error therefore permits a mixture of old and new borrowed
ranges. Keeping both ranges mapped through native release preserves their
storage lifetime; treating failure as unchanged state does not.
[Per-XCC transition][spm-header] [Rotation loop][spm-rotate]

`has_data_loss` is conservative. The driver sets it when a destination is absent
or full and explicitly describes it as a hint that a stall or loss might occur.
It also marks copy failures and overflow interrupts. It neither counts lost
samples nor establishes that every marked interval actually lost data.
[Read and copy][spm-copy] [Interrupt handling][spm-interrupt]

Native acquisition, process ownership, and counter programming remain separate
boundaries. The request dispatcher assigns the node's `spm_pasid` before the
fallible acquisition. An acquisition failure is therefore not evidence of zero
native progress or unchanged routing. Device-node access and an ioctl number
alone do not establish safe concurrent use by competing profilers.
[Native dispatch][spm-dispatch]

[pal-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L1954-L2017
[pal-reader]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSessionPerfSample.cpp#L601-L706
[spm]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c
[spm-uapi]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/hsakmt/linux/kfd_ioctl.h#L1173-L1261
[mainline-uapi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h
[spm-monitor]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L78-L146
[spm-copy]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L161-L308
[spm-acquire]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L327-L415
[spm-release]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L468-L522
[spm-header]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L524-L570
[spm-rotate]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L572-L702
[spm-dispatch]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L704-L740
[spm-interrupt]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_spm.c#L741-L780
[interval-admission]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/spm/service.cpp#L39-L53
[interval-validation]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/spm/service.cpp#L111-L118
[interval-filter]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/spm/service.cpp#L348-L362
[factory-ids]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/pm4_factory.h#L70-L86
[interval-enumeration]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/spm_v2.cpp#L281-L311
[interval-build]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/spm_v2.cpp#L549-L566
[interval-fields12]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/linux/registers/gc/gc_12_0_0_sh_mask.h#L18644-L18654
[interval-encode12]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/gfxip/gfx12/gfx12_primitives.h#L547-L559
[interval-normalize]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/spm_v2.cpp#L270-L278
[interval-encode11]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/gfxip/gfx11/gfx11_primitives.h#L606-L611
[device-start]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/spm/device_counting.cpp#L335-L388
[device-stop]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/spm/device_counting.cpp#L434-L457
[collector-start]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/spm_v2.cpp#L94-L140
[builder-start]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/spm_builder.h#L123-L145
[collector-bridge]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/aql_packet.cpp#L488-L527
[builder-stop]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/spm_builder.h#L744-L785
[collector-drain]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/spm_v2.cpp#L737-L804
[device-controls]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/spm/device_counting.cpp#L259-L266
[native-controls]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/ioctl.cpp#L100-L212
