# Firmware dispatch logs

The vendor KFD dispatch-log service directs compute firmware to write dispatch
start and end records into a kernel-owned buffer. A host maps that buffer and
collects records while queues continue running. The record identifies a queue
and dispatch and carries raw GPU ticks; it is separate from a completion-signal
timestamp and from programmable counter collection. The cited ROCprofiler
consumer uses it to obtain AQL dispatch timing without a separate profiling
completion signal on each traced dispatch. [Native service][open]
[Record format][record] [Timing consumer][timing]

## Applicability and native admission

This interface belongs to the pinned ROCm AMDGPU driver, with profiler protocol
version 6 and dispatch-stream ABI version 3. It is not implied by the presence
of `/dev/kfd`. The driver selects physical GC IP and checks the MEC pipe count:

| Physical GC IP | Regions in one stream | Selection |
| --- | --- | --- |
| 9.5.0 | 4 | One region per MEC pipe. |
| 12.0.0, 12.0.1 | 2 | One region per MEC pipe. |
| Other revisions | None | The cited native selector returns unsupported, including 9.4.x, 11.x and 12.1.0. |

The open path has no firmware-version check. Its architecture predicate
therefore does not establish that any installed firmware implements the
record-writing contract. [ABI][uapi] [Physical selector][selector]
[Open admission][open]

One live stream belongs to a target process and GPU. Self-profiling passes the
native target-authorization check; cross-process collection has additional
device, credential, capability and dumpability checks. Opening the stream
allocates storage and programs existing compute queues; it is not a passive
capability query. The native process owner also applies the stream to newly
created compute queues. [Authorization][authorization]
[Session binding][bind] [Queue creation][autoarm]

Both PM4 and AQL queues map to KFD's internal `COMPUTE` queue type, which is
the binding predicate. That establishes which queues receive MQD fields. The
cited timing consumer operates on AQL dispatch indices; it does not establish
the dispatch-ID or timestamp semantics of arbitrary PM4 packets. SDMA queues
are outside this binding predicate. [Queue type and format][queue-format]
[Binding predicate][bind] [Record interpretation][record]

## Stream representation

`AMDKFD_IOC_PROFILER` uses ioctl number `0x28`, a 40-byte request and operation
`KFD_IOC_PROFILER_DLOG` (`4`). The 24-byte DLOG payload selects `OPEN_STREAM`
(`0`), GPU, target TGID, `RAW_MMAP` and record-region size. Success returns an
anonymous stream file descriptor. `KFD_DLOG_STREAM_IOC`, number `0x88`, applies
to that descriptor and exposes INFO and STATUS; it is not an ioctl on
`/dev/kfd`. INFO supplies the actual offsets and mapping size. [Request][uapi]
[Profiler ioctl number][profiler-ioctl]
[Stream ABI][stream-uapi] [ABI layout checks][abi-checks]

For `R` regions and `C` records per region, the requested record-region size
is `B = R × C × 20` bytes, with `C` a power of two. The native layout is:

| Byte offset | Storage | Writer |
| --- | --- | --- |
| `0` | `B` bytes of records, grouped by region. | Firmware. |
| `B` | `R` 64-bit write counters. | Firmware. |
| `B + 8R` | `R` 64-bit read counters. | Consumer. |
| `B + 16R` through page-rounded end | Padding. | No record payload. |

The cited driver caps requested record storage at 16 MiB and aggregate pinned
GTT across streams at 256 MiB, including the page-rounded control tail. The
SDK's size policy permits larger requests, so its nominal maximum is not the
native admission limit. Both sources accept the SDK's default 10 MiB geometry.
[Layout][layout] [Per-stream cap][limits] [Aggregate cap][aggregate]
[SDK size policy][sdk-size]

The backing is kernel-owned GTT, pinned and mapped through GART. The driver
zeros it and issues `dma_wmb()` before arming firmware. The only mapping mode
requires the complete allocation, zero offset and shared read/write access;
executable mappings and a second simultaneous mapping are rejected. Thus this
native stream does not expose caller-provided GPU backing or multiple virtual
aliases of its ring. The mapping includes firmware-owned write counters even
though the consumer's writable state is only the read counters.
[Allocation][allocation] [Mapping][mapping] [Counter ownership][stream-uapi]

Each little-endian firmware record is 20 bytes:

| Byte offset | Field | Interpretation used by ROCprofiler |
| --- | --- | --- |
| `0`, `4` | `ts_lo`, `ts_hi` | Low and high halves of a 64-bit GPU timestamp. |
| `8` | `record_type` | `0` padding, `1` dispatch start, `2` end of pipe. |
| `12` | `dispatch_id` | Low 32 bits of the HSA queue write index. |
| `16` | `doorbell_off` | Native queue identity. The GFX12 consumer interprets this in DWORD units. |

The consumer skips padding and zero doorbell identity, and rejects unknown
record types. It resolves the doorbell to a page-relative slot separately from
its queue-lifetime history. Low dispatch-index bits and a reused doorbell are
not globally unique identities. [Record][record] [Doorbell units][doorbell]
[Pairing][pairing]

## MQD fields and queue activation

The native queue owner programs VMID0/GART addresses, rather than process GPU
virtual addresses. `dispatch_record_buffer_size` is the total number of
20-byte records across all regions, not bytes or a per-region count. The
notify interval is 50 records when armed and zero when disabled.
[Queue properties][queue-properties] [Notify policy][limits]

| MQD field | GC 9.5.0 DWORD | GC 12.0.x DWORD |
| --- | --- | --- |
| `dispatch_record_buffer_addr_lo`, `dispatch_record_buffer_addr_hi` | 43, 44 | 310, 311 |
| `dispatch_record_buffer_size` | 45 | 312 |
| `dispatch_record_wptr_addr_lo`, `dispatch_record_wptr_addr_hi` | 48, 49 | 313, 314 |
| `dispatch_record_notify_interval` | 46 | 315 |

The GFX9 builder zeroes reserved DWORDs 47 and 50–53. Both builders program
zero addresses during unbind; omitting the update when the new base is zero
would leave a queue armed. Binding an existing queue invokes the native queue
update, while creation can install the fields before the initial MQD load.
[GFX9 offsets][mqd9-layout] [GFX9 builder][mqd9-builder]
[GFX12 offsets][mqd12-layout] [GFX12 builder][mqd12-builder]
[Queue binding][autoarm]

## Publication, overwrite and collection

The UAPI specifies free-running unsigned 64-bit counters, with the slot selected
by the low bits of the count. Firmware publishes payload before its write
counter. A reader observes that counter, performs the required read ordering,
then accesses the published payload. The native contract uses unsigned
`write - read` for the unread distance; ordinary numerical comparisons do not
preserve this rule across wrap. [Counter protocol][stream-uapi]

Firmware never reads the consumer counters and never waits for credits. If
the unread distance exceeds capacity, some history has already been overwritten.
Advancing the read counter acknowledges consumption for host readiness; it
does not prevent the next firmware write. A retained pointer into this mapping
therefore does not lease an immutable record. [Overwrite protocol][stream-uapi]

The SDK copies records to separate CPU storage and reloads the producer counter
after the copy. For an unwrapped counter, it marks copied indices at or below
`write_after_copy - capacity` untrusted, including the next slot that could be
under overwrite at exactly-full occupancy. It discards these records before
pairing. This is distinct from counting records already lost to a completed
lap. The algorithm also treats numerical counter regression as invalid; that
runtime choice differs from the UAPI's unsigned-wrap rule. [Copy algorithm][copy]

That overwrite check has a firmware premise: the published cursor must bound
in-flight payload writes, with at most the immediately following slot exposed
by the cited check. Payload-before-cursor publication alone does not bound how
far a pipelined writer can prepare later slots before publishing its cursor.
The cited UAPI states publication order and one writer per counter, but does
not specify that preparation distance. The SDK algorithm documents its chosen
model; those statements alone do not establish a general stable-snapshot
protocol for a different firmware writer. [Publication contract][stream-uapi]
[Consumer check][copy]

Records from different regions can arrive in collection order different from
timestamp order. The SDK orders copied records by ticks before matching start
and end records. Duplicate outstanding raw identities are ambiguous; missing
starts, equal timestamps and stale ends have explicit handling. Such policy
cannot recover information discarded by overwriting or identity truncation.
A valid end record without a start supplies no measured interval.
[Pairing and ambiguity][pairing]

The SDK passes firmware ticks to
`hsa_amd_profiling_convert_tick_to_system_domain`, which delegates to ROCr's
GPU/host clock translation. The stream record and INFO structure contain no
frequency, epoch or paired host sample. Using that conversion therefore relies
on the firmware source sharing the clock domain expected by ROCr; field width
alone does not establish it. [Timing caller][timing]
[Translation](../observability.md#clock-domains-and-conversion)

## Notification and completed storage use

Firmware notifications use an EOP interrupt with a dispatch-log context tag and
the target PASID. KFD routes it to the matching stream waitqueue. `poll()`
registers the waiter before checking for `write != read`, so readiness is level
based. Queue destruction provides an additional drain notification for a tail
shorter than the notification interval. A wake is neither a retained record
nor a timestamp for the observed dispatch. [Interrupt routing][notify]
[Readiness][poll]

The stream has no explicit stop-and-freeze operation. The file's final release
unbinds queues, but a live mapping holds a file reference: closing only the
descriptor does not stop collection while retaining a readable frozen view.
The SDK unmaps and then closes. An exact final-tail protocol consequently needs
the surrounding queue/target lifetime to establish the last firmware writer;
close alone does not provide a stopped snapshot to the caller.
[Available operations][stream-ops] [File release][release]
[SDK cleanup][cleanup]

If native unbind fails, KFD retains the session and backing while any MQD may
still reference it; `UNBIND_RETAINED` describes this condition. A later open
can retry removal of a terminal retained session. Target exit follows queue
destruction, publishes HUP, and leaves geometry and mapped storage available
for final reading. A GPU reset publishes ERR without declaring normal capture
completion or freeing the buffer. These are distinct terminal conditions.
[Unbind and retention][unbind] [Retained-session retry][bind]
[Target exit][target-exit] [Reset][reset]

[uapi]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/include/uapi/linux/kfd_ioctl.h#L1748-L1813
[profiler-ioctl]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/include/uapi/linux/kfd_ioctl.h#L2069-L2070
[stream-uapi]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/include/uapi/linux/kfd_ioctl.h#L1875-L1944
[open]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L991-L1112
[selector]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L778-L804
[authorization]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L806-L886
[bind]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L944-L1041
[autoarm]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L343-L422
[queue-format]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L299-L314
[abi-checks]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/dlog_abi.cpp#L43-L99
[layout]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L148-L180
[limits]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L1059-L1063
[aggregate]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L58-L75
[sdk-size]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/dlog_drain.hpp#L52-L79
[allocation]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L182-L217
[mapping]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L729-L776
[record]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/dlog_drain.hpp#L94-L107
[doorbell]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/doorbell_map.hpp#L64-L78
[queue-properties]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L332-L341
[mqd9-layout]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L42-L60
[mqd9-builder]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L183-L212
[mqd12-layout]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L35-L52
[mqd12-builder]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L275-L292
[copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/dlog_drain.hpp#L274-L390
[pairing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/dlog_drain.hpp#L393-L533
[timing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue_interposition.cpp#L529-L582
[notify]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L336-L422
[poll]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L834-L896
[stream-ops]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L622-L665
[release]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L779-L829
[cleanup]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/kfd/kfd_reader.cpp#L475-L505
[unbind]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L1043-L1093
[target-exit]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L517-L570
[reset]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_dlog_stream.c#L424-L439
