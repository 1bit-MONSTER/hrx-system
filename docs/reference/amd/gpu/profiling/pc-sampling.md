# Program-counter sampling

Program-counter sampling records a running wave's instruction location and
execution state. In ROCr's implementation, a kernel service triggers sampling
and a GPU trap handler writes records into two reusable buffers per XCC. The
sampling source, record writer, buffer publication and trap retirement have
different completion boundaries. [Native source][native-source]
[Trap installation][trap-install] [Buffer layout][buffer-layout]

## Native source and applicability

The cited vendor KFD implementation reports PC-sampling protocol version 1.9.
Its capability table uses physical GC IP versions, with the following method
and interval combinations. These are native-driver predicates, not a mapping
from compiler target names or a guarantee for other driver revisions.
[Version and capability table][native-capabilities]

| Physical GC IP | Host-trap interval | Stochastic interval |
| --- | --- | --- |
| 9.4.2 | At least 1 microsecond | No entry in this table |
| 9.4.3, 9.4.4, 9.5.0 | At least 1 microsecond | Power of two, 256 through 2^31 core clock cycles |
| 12.0.0, 12.0.1 | At least 512 microseconds | No entry in this table |
| 12.1.0 | At least 512 microseconds | Power of two, 256 through 2^20 core clock cycles |

The driver rejects revision A0 of GC 12.1.0 before dispatching any operation.
It also rejects the no-HWS scheduling policy. Capability-query and creation
requests conflict with an enabled process debug trap. [Revision check][native-capabilities]
[Scheduling requirement][native-entry] [Operation dispatch][native-dispatch]

Host-trap sampling uses a kernel thread to trigger selected SIMD/wave slots.
The advertised maximum interval is `UINT64_MAX`, but this implementation casts
the interval to a 32-bit microsecond timeout inside the thread. Values above
`UINT32_MAX` therefore do not retain the requested interval; a resulting zero
timeout exits the thread. The capability's 64-bit range and this consumer's
32-bit interval are distinct facts. Stochastic sampling instead programs the
native sampling source. [Trigger loop][native-trigger] [Start][native-start]

`kfd_ioctl_pc_sample_args` is 32 bytes: an eight-byte `sample_info_ptr`, followed
by 32-bit `num_sample_info`, `op`, `gpu_id`, `trace_id`, `flags` and `version`.
The pointed-to `kfd_pc_sample_info` is 40 bytes: four 64-bit interval/minimum/
maximum/flags fields and two 32-bit method/type fields. Operations are query,
create, destroy, start and stop, numbered 0 through 4. Host trap and stochastic
methods are 1 and 2. The header also defines an instruction interval unit, but
the cited capability table has no instruction-based entry. [Native ABI][native-abi]

The full-query flag asks for the table's possibilities. Without it, existing
users can restrict the returned configurations. Creation shares a device's
method configuration when the complete requested configuration matches; a
conflict returns `EEXIST` and copies the current configuration back. A trace ID
belongs to its creating process/device. Destroy rejects an enabled trace.
[Query][native-query] [Creation][native-create] [Identity and transitions][native-dispatch]

## Trap writer and record representation

The source control alone does not install a record writer. ROCr installs
second-level trap code and a TMA descriptor containing separate host-trap and
stochastic buffer pointers plus the per-XCC byte stride. The code, descriptor,
sample storage and notification objects remain accessible while a trap can
use them. The trap also handles ordinary exceptions; PC sampling is not an
independent shader entry point. [Trap installation][trap-install]
[Descriptor layout][buffer-layout] [Trap exit][trap-publish]

ROCr's two version-1 record layouts are both 64 bytes:

| Byte offset | Representation |
| --- | --- |
| `0x00` | 64-bit program counter. |
| `0x08` | 64-bit EXEC mask. |
| `0x10`, `0x14`, `0x18` | 32-bit workgroup X, Y and Z. |
| `0x1c` | Wave-in-workgroup in bits 5:0; reserved bits 7:6; chiplet in bits 10:8; remaining bits reserved. |
| `0x20` | 32-bit native hardware ID. |
| `0x24`–`0x2f` | Reserved for host-trap records; three 32-bit performance-snapshot words for stochastic records. |
| `0x30` | 64-bit timestamp read by the trap handler. |
| `0x38` | 64-bit native correlation value. |

[Record declarations][records] [GFX9 writer][trap-record]

GFX9 reads `S_MEMREALTIME`; GFX12 uses `MSG_RTN_GET_REALTIME`. The timestamp
is taken in the handler and is not the host's trigger time or an instruction
duration. The [clock discussion](../observability.md#shader-realtime-and-frequency)
describes the source distinction; a stored 64-bit value does not establish a
host epoch or a frequency conversion. The correlation value combines a native
doorbell identity with a truncated dispatch index. Queue reuse and index wrap
require the surrounding submission history to resolve that value to a unique
dispatch. [GFX9 timestamp and correlation][trap-record]
[GFX12 timestamp][trap12-timestamp] [GFX12 correlation][trap12-correlation]

Host-trap and stochastic records do not obtain PC identically. The GFX12
stochastic handler reads performance-snapshot PC registers. Its compiler
predicate for generation minor at least 5 reads snapshot `PC_HI` last to
release the snapshot lock; the minor-0 path reads snapshot `DATA` last for
its valid-bit protocol. These are ordering requirements of the selected
handler, not interchangeable read sequences. [Snapshot order][trap12-snapshot]

## Buffer publication during execution

Each XCC has a 64-byte control header followed by two arrays of records. The
TMA descriptor's stride locates the next XCC's header and arrays.
[Buffer layout][buffer-layout]

| Header byte offset | Field and meaning |
| --- | --- |
| `0x00` | 64-bit `buf_write_val`: bit 63 selects the buffer; bits 62:0 count reservations. |
| `0x08` | 32-bit `buf_size`, measured in records per buffer. |
| `0x0c` | Reserved 32-bit field. |
| `0x10`, `0x20` | 32-bit `buf_written_val0/1`, completed records for each buffer. |
| `0x14`, `0x24` | 32-bit watermark for each buffer's notification. |
| `0x18`, `0x28` | Eight-byte HSA signal handles for the two notifications. |
| `0x30`–`0x3f` | Reserved padding. |
| `0x40` | First record of buffer 0; buffer 1 follows `buf_size` records later. |

A trap atomically increments the packed reservation word, retaining the old
buffer selection and index. Reservations beyond capacity produce no record.
An admitted writer stores its record before incrementing that buffer's
completed-record count. GFX9 executes its scalar-data writeback and wait before
the increment; the GFX12 writer waits for stores before its completion path.
The watermark notification follows the completion increment. The pinned GFX9
handler compares the atomic's previous count against the watermark, whereas
the GFX12 handler compares the incremented count. The same raw watermark
therefore signals one completion later on that GFX9 path. GFX12 also selects
DEVICE scope for its completion atomic when generation minor is at least 5,
and SYSTEM scope otherwise; transferring this protocol to another allocation
or observer needs the corresponding memory-visibility contract.
[Reservation][trap-reserve] [GFX9 publication and notification][trap-publish]
[GFX12 publication][trap12-publish]

ROCr's live rotation uses this sequence:

1. The collector atomically replaces the reservation word with the other
   buffer's selector and a zero reservation count. Later reservations use the
   other buffer; the returned word bounds the old buffer's writers.
2. It caps the old reservation count at capacity and accounts for excess
   reservations as lost samples.
3. It waits until the old buffer's completed count reaches that capped count.
   Only then are all admitted records in the sealed buffer complete.
4. It copies the completed extent, resets that buffer's completed count and
   eventually returns the buffer to the producer through another rotation.

The CPU-accessible path uses an atomic exchange and an acquire load. The
non-large-BAR path uses PM4 `ATOMIC_MEM`, confirmed return-value readback,
`WAIT_REG_MEM`, copy and reset commands. Multi-XCC carriers select one executor
for these memory operations; this is separate from the XCC that wrote the
records. [CPU rotation][cpu-rotation] [PM4 rotation][pm4-rotation]

A partial completed count is not a contiguous readable prefix. For example,
writer 0 can reserve slot 0 and pause while writer 1 completes slot 1. A count
of one then does not make slot 0 valid. Sealing the buffer and waiting for
*all* admitted writers resolves that ordering problem. This follows from the
reservation and completion algorithms, rather than from a guarantee that waves
finish in reservation order. [Reservation][trap-reserve]
[Publication][trap-publish] [Collector wait][cpu-rotation]

The device buffers and the collector's host ring have separate overflow
conditions. The cited collector counts excess device reservations, and drops
new copied data when its host ring lacks space. Its notification threshold is
not a credit that prevents the device from exhausting its current buffer.
[Loss accounting][cpu-rotation]

## Stop, final collection and storage lifetime

KFD start marks the trace enabled, updates its trap flag and increments active
users before a fallible host-trap thread creation. A failed start can therefore
leave native progress. Stop clears the flag and active count, requests a queue
remap, and stops the source when its last active user leaves. Destroy removes
the inactive trace's registration. [Start][native-start]
[Stop][native-stop] [Destroy][native-destroy]

The cited `remap_queue` implementation calls `execute_queues_cpsch` only on
the non-MES path and discards its return value. The trap-flag setter updates
the process's CWSR metadata. Together, these functions do not supply an
explicit, error-reporting acknowledgement that every second-level trap has
returned on every scheduling path. Stopping the host trigger thread also
differs from joining already running GPU handlers. [Remap][native-remap]
[Flag update][native-flag] [Trigger loop][native-trigger]

ROCr calls native stop, marks the session inactive, joins its collector
threads, and performs final collection. Its CPU rotation can abandon the
full-count wait when the session becomes inactive, using the observed completed
count as the copy length. The prefix example above means that this shortcut
depends on an additional producer-quiescence guarantee. The native sources
cited here do not establish that guarantee for every return path, and the
ROCr stop implementation marks the session inactive even if native stop fails.
This is an unresolved cross-layer completion contract, not evidence that a
partial count itself publishes a valid prefix. [Runtime stop][runtime-stop]
[Inactive collection path][cpu-rotation]

Record completion also precedes the handler's signal and mailbox accesses.
Consequently, a completed sample buffer can be consumed while the trap code,
TMA and notification storage still have users. Replacing the trap descriptor
or releasing those objects needs their final-use boundary in addition to the
record-count observation. A queue fence on a collector's copy work retires
that copy; it does not by itself join every trap on participating queues.
[Notification after publication][trap-publish]
[Trap descriptor replacement][trap-install]

[native-source]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c
[native-capabilities]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L33-L91
[native-abi]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/include/uapi/linux/kfd_ioctl.h#L1692-L1747
[native-entry]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L2002-L2034
[native-trigger]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L93-L166
[native-query]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L186-L266
[native-create]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L364-L472
[native-start]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L268-L319
[native-stop]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L321-L362
[native-destroy]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L474-L499
[native-dispatch]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c#L527-L580
[native-remap]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L3978-L3986
[native-flag]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L1808-L1830
[buffer-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_agent.h#L935-L978
[trap-install]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3331-L3387
[records]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ven_amd_pc_sampling.h#L57-L97
[trap-reserve]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s#L403-L475
[trap-record]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s#L498-L642
[trap-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s#L645-L710
[trap12-timestamp]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler_gfx12.s#L682-L732
[trap12-correlation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler_gfx12.s#L839-L898
[trap12-snapshot]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler_gfx12.s#L924-L973
[trap12-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler_gfx12.s#L909-L1045
[cpu-rotation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4545-L4674
[pm4-rotation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4676-L4945
[runtime-stop]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4480-L4543
