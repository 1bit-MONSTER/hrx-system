# PM4 queue publication and doorbells

A PM4 producer initializes a stream of DWORDs, then publishes a frontier that
makes those commands available to the command processor. The CP can consume
an INDIRECT_BUFFER reference and launch shader or DMA work before that work
finishes. Command publication, ring consumption, and completed access to the
referenced storage are separate events.

## Native transports and pointer units

Three native submission paths have different owners:

| Transport | Command producer and publication owner | Completion surrounding the commands |
| --- | --- | --- |
| KFD raw compute queue | Userspace owns the PM4 ring and writes the native WPTR and mapped doorbell. `KFD_IOC_QUEUE_TYPE_COMPUTE` selects PM4, separately from `COMPUTE_AQL`. | The producer supplies its execution dependencies and completion commands. |
| Scheduled DRM submission | Userspace supplies GPU-addressed IBs, resource lists and dependencies; the kernel owns the hardware ring and publishes its wrapper. | The kernel emits VM/cache work and fences around the submitted IBs. |
| DRM user queue | Userspace owns the ring; the driver supplies queue configuration, protected completion storage and dependency integration. | Mesa emits waits, an IB, an ordinary user fence and a protected fence, then associates the submission with native synchronization objects. |

[KFD queue selection][kfd-type] [Scheduled wrapper][linux-wrapper]
[DRM userq producer][mesa-userq-body] [DRM userq submission][mesa-userq-submit]

The KFD queue utility below is a concrete serialized producer. Its padding and
space-accounting choices are not a universal PM4 ring algorithm. Similarly,
the DRM userq protocol requires that native interface and its firmware; its
protected packets and VM inheritance are not an implicit KFD service.

| Quantity | Representation in the cited paths |
| --- | --- |
| Ring address and size | KFD receives a ring byte address and byte size. Its V9/V11 MQD builders encode the base in 256-byte units and derive capacity from `queue_size / 4`. DRM userq receives a GPU virtual byte address and byte size. |
| KFD raw PM4 progress | RPTR is a 32-bit DWORD position wrapping at ring capacity in the utility. Its pre-Vega10 branch publishes a 32-bit wrapped WPTR; Vega10 and later publish a cumulative 64-bit DWORD WPTR. |
| Doorbell value | The matching KFD branches write the same published DWORD frontier through a 32- or 64-bit doorbell. The doorbell mapping is returned by the native queue interface. |
| GFX11 scheduled compute progress | Linux reads a 32-bit RPTR, stores the 64-bit WPTR in memory, then writes the 64-bit doorbell. Ring indexing separately masks the DWORD position by capacity minus one. |
| Mesa DRM userq progress | A 64-bit cumulative DWORD cursor indexes a power-of-two ring. The ordinary user fence stores a completion value expressed in the same cumulative DWORD units. |
| IB address and length | PM4 INDIRECT_BUFFER carries a GPU byte address and a DWORD count. `drm_amdgpu_cs_chunk_ib` instead carries `va_start` and `ib_bytes`; PAL, RADV and Linux explicitly convert between bytes and DWORDs. |

[KFD native arguments][kfd-args] [V9 MQD][linux-mqd9]
[V11 MQD][linux-mqd11] [KFD pointer branches][kfd-publish]
[Linux compute pointers][linux-compute-pointers] [Linux ring indexing][linux-index]
[Mesa userq cursors][mesa-userq-cursors] [DRM IB structure][drm-ib]
[PAL conversion][pal-ib] [RADV conversion][radv-ib] [Linux conversion][linux-ib-input]

A modulo ring offset identifies storage, not a unique submission. A cumulative
frontier also needs a bounded outstanding extent so that the producer cannot
lap live commands. Neither representation makes a multi-producer reservation
protocol by itself. AQL's 64-byte packet indexes and header publication follow
a [different protocol](../aql/publication.md).

## KFD raw compute publication

The KFD utility requests a zeroed, executable system-memory ring with its
`isUncached` allocation flag and passes the address and byte size to queue
creation. The thunk supplies the raw queue's pointer storage and doorbell
mapping. Linux accepts a power-of-two requested ring size or zero before
applying a minimum-size clamp; the native MQD base encoding also requires the
selected ring alignment. Allocation, native queue capacity, and the
producer's arithmetic must describe the same backing.
[Ring owner][kfd-ring-owner] [Pointer and doorbell owners][kfd-args]
[Size handling][linux-kfd-size] [Base encoding][linux-mqd9]

The request's GPU cache policy and the CPU mapping's cacheability remain
distinct. The native flag realization and GTT mapping composition are described
with [command-storage ownership](command-buffers.md#publication-and-memory-ownership).
The host barriers below order stores in that mapping; they are not a substitute
for establishing GPU access or the required mapping policy.

The [KFD queue-storage contract](../architectures.md#kfd-queue-storage) also
requires the ring and control words to resolve through native BO mappings.
The separate SVM context-save path does not extend to the ring.

`BaseQueue::PlacePacket` separates construction from publication. Let `C` be
ring capacity in DWORDs, `R` the reported modulo RPTR, `W` the producer's
pending modulo position, and `N` the packet size. It leaves one DWORD unused:

```text
available = (R - 1 - W + C) mod C
required  = N + (W + N > C ? C - W : 0)
```

The complete reservation includes any tail padding. When a packet would cross
the ring end, the producer fills the remaining tail with its PM4 NOP encoding,
wraps, and copies the complete packet contiguously. A packet ending exactly
at the boundary needs no tail padding. Both the modulo cursor and cumulative
cursor advance over every padding and packet DWORD. Placement alone does not
update native WPTR. [Reservation and placement][kfd-reserve]

For this producer, the complete publication sequence is:

1. Serialize access to the queue object's pending cursors. Establish space for
   the complete packet and any padding using a current RPTR observation.
2. Initialize every newly published ring DWORD, and complete writes to any
   externally referenced IB, arguments, code and input storage according to
   those allocations' visibility requirements.
3. Execute the host publication barrier, store the pending native WPTR, execute
   the second host barrier, then write that same frontier to the mapped
   doorbell. The Linux utility implements both barriers with
   `__sync_synchronize()`; the pointer and doorbell widths follow its family
   branch above.
4. Keep the published command range unchanged until it is consumed. Keep
   separately referenced resources through their final execution user and
   completion observation.

The source has one mutable pair of pending cursors and no concurrent producer
reservation mechanism. The serialization requirement follows from that owner
shape; it is not a hardware promise about interleaved host writes.
[Construction state][kfd-reserve] [Publication order][kfd-publish]
[Host barrier implementation][kfd-host-barrier]

### Memory WPTR can be observed before a new doorbell

The native memory WPTR is itself a publication surface. In Linux's V9 and V11
KFD HQD-load paths, the driver activates the doorbell logic and asks the CP to
perform a **one-shot polling read** of the supplied WPTR address. The comments
require that address to be GPU-accessible in the queue's VMID through ATC or
SVM. The driver reconstructs an initial 64-bit WPTR from the saved state and
32-bit RPTR under a no-overflow assumption, then starts fetching from the
proper position. [V9 load][linux-load9] [V11 load][linux-load11]

Consequently, delaying only the doorbell cannot hide an already advanced
memory WPTR from queue loading or restoration. Initialized command visibility
must precede the WPTR store. This source establishes the load-time one-shot
read, not continuous polling on every PM4 queue: Linux's separate GFX11 kernel
compute initialization explicitly disables `CP_PQ_WPTR_POLL_CNTL` polling.
[Kernel compute initialization][linux-poll-disable]

### Completion and removal

The utility explicitly distinguishes consumed packets from processed work.
Its RPTR path waits for the expected modulo ring position; its event path
emits a release and waits for the native event. An RPTR report can lag and
does not establish shader-result visibility. [Consumption contract][kfd-consumed]
[Observation paths][kfd-publish]

The ordinary shader-copy caller builds an IB, submits its reference, and
emits an event-producing release on the primary ring. Its IB separately joins
the shader with a RELEASE_MEM fence and WAIT_REG_MEM. The caller waits for
the event and then destroys the queue before its dispatch, code and argument
owners leave scope. The queue owner releases the ring only after successful
native destruction; the thunk likewise retains its queue object when removal
fails. [Dispatch and event][kfd-dispatch] [In-body shader join][kfd-join]
[Caller lifetime][kfd-caller] [Ring removal][kfd-destroy]
[Thunk removal][kfd-thunk-destroy]

This is an explicit raw-queue completion flow. A scheduled DRM trailer is not
added to it. The applicable shader completion and cache operations are
described in [memory commands](memory-commands.md#compute-completion-and-firmware)
and [cache control](cache.md).

## Scheduled DRM publication

In scheduled submission the application's command storage is an IB allocation,
not the native hardware ring. The request supplies its GPU byte address,
byte length, engine and ring selection. Native HW-IP information supplies IB
start and size alignments. RADV applies its per-IP padding mask, emits valid
NOPs even for an otherwise empty IB, and reserves chain space when needed.
These are IB constraints, separate from the kernel ring's fetch-size padding.
[IB request][drm-ib] [Native alignment information][drm-ip-info]
[RADV finalization][radv-finalize]

Placement and cacheability belong to the allocator path. Mesa's Gallium
producer chooses CPU-cached GTT with GL2 bypass for command buffers. RADV can
select GTT or VRAM and requests write-combined GTT except for its secondary
buffer path that must be copied instead of using graphics IB2. These concrete
choices do not establish that every CPU-visible allocation has the same
publication contract. PAL finalization copies commands from staging into the
mapped GPU allocation when those pointers differ.
[Gallium command storage][mesa-ib-memory] [RADV placement][radv-ib-memory]
[PAL finalization][pal-finalize]

The ordinary scheduled flow is:

1. Finish command bytes in the selected allocation, including valid padding
   and every referenced command chunk. Supply the resource list and native
   synchronization dependencies with the IB request.
2. The kernel validates the request and retains the GPU IB address and DWORD
   length. The ordinary GFX11 compute path does not copy the body into its
   hardware ring; it emits INDIRECT_BUFFER with the job's VMID.
3. The kernel reserves the wrapper's DWORD budget, handles required VM and
   pipeline synchronization, performs HDP/cache work selected by the native
   path, emits the IB references, and appends completion fences.
4. It pads the ring to its fetch granularity, executes `mb()`, stores WPTR and
   rings the doorbell. The GFX11 compute implementation uses a 64-bit memory
   WPTR and doorbell write.
5. The client observes the submitted work's finished fence before modifying
   command bytes or resources still referenced by that submission. Returning
   from the submission ioctl establishes neither execution completion nor
   permission to rewrite the IB.

[PAL resources and submit][pal-submit] [Linux IB input][linux-ib-input]
[GFX11 IB emission][linux-compute-ib] [Native wrapper][linux-wrapper]
[Ring commit][linux-commit] [Compute WPTR][linux-compute-pointers]
[Finished fences on resources][linux-resource-fences]

### Ring capacity and wrapper ownership

Linux bounds a single hardware-ring submission by `max_dw`, including
alignment padding. The ring allocation is a power-of-two size derived from
that budget and the number of hardware submissions; the scheduler receives
the corresponding credit limit. `amdgpu_ring_alloc` records the reserved
DWORD budget rather than polling RPTR for free space. Copying that function
alone would omit the scheduler's capacity premise.
[Reservation][linux-ring-reserve] [Capacity][linux-ring-capacity]
[Scheduler credits][linux-credits]

The kernel writer wraps individual DWORD stores with a capacity mask. Commit
adds NOPs to match the engine's fetch alignment; GFX11 compute uses alignment
mask `0xff`, or 256 DWORDs. This differs from the KFD utility's policy of
padding before a packet that would straddle the ring end.
[Ring indexing][linux-index] [Commit padding][linux-commit]
[GFX11 ring operations][linux-compute-ring]

The completion wrapper is equally transport-specific. The GFX11 kernel fence
builder emits RELEASE_MEM with `CACHE_FLUSH_AND_INV_TS`, event index 5,
GL2 writeback, GLM writeback/invalidate, sequential cache control and cache
policy 3, followed by the selected 32- or 64-bit fence data. Linux associates
finished fences with the submission's locked BO reservations. Those actions
are evidence for this scheduled path, not a property of merely executing an
IB on another native queue. [GFX11 fence][linux-compute-fence]
[Resource completion][linux-resource-fences]

## DRM compute user queues

DRM userq moves ring publication into userspace but retains driver-mediated
VM, synchronization and protected-fence ownership. Native queue creation
receives the ring GPU VA, a queue size that is a multiple of 256 bytes,
eight-byte-aligned RPTR/WPTR storage, a doorbell object and an engine-specific
MQD parameter record. The compute record names a separate EOP allocation.
In the MES implementation, WPTR has
an additional GART mapping and an eviction-fence-held BO reference; that path
restricts the WPTR BO to at most one page. The generic UAPI permission to share
a BO among queue and pointers does not erase this implementation restriction.
[Creation fields][drm-userq-create] [Compute EOP record][drm-userq-eop]
[WPTR mapping owner][linux-userq-wptr] [MES queue inputs][linux-mes-input]

Mesa allocates a 64-KiB GTT ring plus a page for its user fence and diagnostic
storage, a separate GTT WPTR page, VRAM RPTR storage, a compute EOP allocation,
and a mapped doorbell BO. Its ring/WPTR allocations request GL2 bypass. It
initializes the host-written cursors to zero and waits for the relevant VM
timeline point before queue creation. The native driver resolves the doorbell
handle and offset; for compute it treats the offset as an index of eight-byte
doorbell slots. The UAPI field is 32 bits wide, not a declaration that the
compute doorbell store is 32 bits.
[Mesa ring owners][mesa-userq-memory] [EOP and creation][mesa-userq-create]
[Native doorbell indexing][linux-userq-doorbell]

### A complete Mesa compute submission

Mesa's ordinary compute producer performs the following sequence:

1. Finalize the separately owned IB. Collect explicit synchronization objects,
   the latest relevant VM-update timeline point and shared-BO dependencies.
   `USERQ_WAIT` returns GPU fence-address/value pairs for command-stream waits.
2. Serialize construction and publication with the queue mutex. Write the
   returned dependencies using FENCE_WAIT_MULTI, then HDP_FLUSH and the main
   INDIRECT_BUFFER. The compute IB sets VALID and `INHERIT_VMID_MEC`; the
   graphics branch instead uses `INHERIT_VMID_PFP`.
3. Append an eight-DWORD RELEASE_MEM to the ordinary user fence and a
   two-DWORD PROTECTED_FENCE_SIGNAL. The user-fence value is the cumulative
   position after both packets, so it agrees with the protected completion
   frontier. The protected signal is last before publication.
4. On the cited GCC-compatible x86/x86-64 path, execute `mfence`, store the
   64-bit memory WPTR, execute `mfence` again, and write the same value to the
   64-bit mapped doorbell. Then issue `USERQ_SIGNAL` to associate that frontier
   with synchronization objects and shared-BO fences before releasing the
   queue mutex.
5. Observe the resulting completion before reusing referenced work storage.
   Mesa first checks its user fence and otherwise waits through the native
   synchronization object. Queue removal has its own native last-fence wait
   and unmapping path.

[Dependencies and publication][mesa-userq-submit]
[Wait, IB and completion construction][mesa-userq-body]
[Host completion observation][mesa-fence-wait]
[Native queue removal][linux-userq-destroy]

The wait builder batches at most 32 dependencies when the device has dedicated
VRAM or is GFX12+, and four otherwise. It selects engine 1, poll interval 4
and preemptable operation. These are this caller's predicates. Its user fence
uses `CACHE_FLUSH_AND_INV_TS`, event index 5, GL2 writeback, sequential cache
control and policy 3; it additionally selects GLM writeback/invalidate below
GFX12. [Exact wait and release construction][mesa-userq-body]

The protected fence differs from a caller-selected memory destination. The
driver places its address in queue state; Mesa's source describes that memory
as writable only through VMID 0 and read-only in the user VMID. The completion
packet records the monotonic ring position after completed work. Dependencies
obtained through USERQ_WAIT refer to that protected storage. An ordinary
WRITE_DATA to a private buffer does not substitute for this driver integration.
[Protected completion contract][mesa-protected-fence]
[Native protected address][linux-userq-mqd]

Capacity accounting also differs from the raw KFD utility. Mesa wraps each
DWORD store with a 16,383 mask and requires `new_WPTR - completed_user_fence`
to fit the 16,384-DWORD ring. Its bounded fence history waits for the oldest
submission before recycling that entry. This producer accounts against a
completed-work frontier, rather than leaving one slot free against a modulo
RPTR. The source's assertion describes an invariant of its allocation and
submission policy, not a general-purpose space-reservation operation.
[Userq ring writer][mesa-userq-cursors] [Fence-history retirement][mesa-fence-history]

### Native and firmware applicability

The cited Linux revision installs GFX/compute DRM userq operations for
GC 11.0.0, 11.0.2, 11.0.3, 12.0.0 and 12.0.1 only when user queues are enabled
and ME/PFP/MEC firmware versions are at least 3090/3190/3450, respectively,
with MES firmware at least 147. Its GC 11.0.1, 11.0.4 and 11.5.x branch is
disabled in that source. The native `userq_ip_mask` reflects installed userq
operations; raw KFD or scheduled-compute availability does not imply that this
DRM userq path is available.
[GFX11 registration][linux-userq-gfx11] [GFX12 registration][linux-userq-gfx12]
[Native mask][linux-userq-mask] [Reported fields][drm-ip-info]

The cited Mesa and Linux snapshots also have different USERQ_SIGNAL
declarations: Mesa includes a trailing `syncobj_points` pointer and a 64-bit
`num_syncobj_handles`; Linux's structure lacks that pointer and uses a 16-bit
count plus padding. Their source paths establish the producer and driver
roles, but not byte-identical ABI compatibility between these two revisions.
A complete native deployment must match its request layout and implemented
driver protocol. [Mesa declaration][mesa-userq-signal-abi]
[Linux declaration][linux-userq-signal-abi]

## Storage reuse boundaries

Publication leaves several independent borrowed resources:

| Resource | Observation needed before reuse |
| --- | --- |
| Previously published ring slots | The transport's consumed or completed frontier has advanced beyond those slots, with wrap accounted for. |
| Referenced IB bytes | All published references have returned or otherwise completed their native command lifetime; any shader/DMA users of command storage have also finished. Ring space alone does not retire an external IB. |
| Code, arguments and payload | Completion of the final operation that can access them, plus the visibility operations needed by the next observer. |
| Completion and dependency storage | The signaling writer and every queued reader have finished. Observing the writer alone does not retire a later consumer's wait. |
| Ring, pointers, EOP and doorbell owners | Successful completion of the native queue-removal ownership path before their backing is released. |

The source flows above supply different observations for these boundaries.
PAL's retained-command reset contract explicitly requires that a buffer is
neither queued nor executing and that every other command buffer which
referenced it through `CmdExecuteNestedCmdBuffers` has also been reset.
Automatic allocator reuse instead uses root generation and submit/done
tracking. These distinctions explain why a completed submission permits a
controlled CPU rebuild, while an advanced RPTR or an accepted submission alone
does not. [Client reset contract][pal-reset]
[Automatic idle tracking][pal-idle] [Allocator reuse][pal-reuse]

[Command-buffer ownership](command-buffers.md) covers the entry/return and
mutable-IB protocols. [Cross-queue handoff](handoff.md) covers dependent payload
users. Neither an in-IB cache command nor a later doorbell can retroactively
publish command bytes that the CP was already allowed to fetch.

[kfd-type]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L281-L310
[kfd-args]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/queues.c#L765-L849
[linux-mqd9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L245-L275
[linux-mqd11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L195-L227
[kfd-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/PM4Queue.cpp#L37-L84
[linux-compute-pointers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L5955-L5987
[linux-index]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.h#L491-L500
[mesa-userq-cursors]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_userq.h#L14-L38
[drm-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L1018-L1032
[pal-ib]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L1939-L1976
[radv-ib]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1752-L1769
[linux-ib-input]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L395-L415
[kfd-ring-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BaseQueue.cpp#L41-L100
[linux-kfd-size]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L234-L242
[kfd-reserve]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BaseQueue.cpp#L160-L207
[kfd-host-barrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/LinuxOSWrapper.cpp#L282-L284
[linux-load9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v9.c#L245-L286
[linux-load11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c#L203-L246
[linux-poll-disable]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L4473-L4492
[kfd-consumed]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/PM4Queue.hpp#L42-L55
[kfd-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/Dispatch.cpp#L82-L104
[kfd-join]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/Dispatch.cpp#L265-L276
[kfd-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDTestUtil.cpp#L293-L323
[kfd-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BaseQueue.cpp#L118-L130
[kfd-thunk-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/queues.c#L920-L941
[drm-ip-info]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L1534-L1557
[radv-finalize]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L450-L507
[mesa-ib-memory]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L717-L747
[radv-ib-memory]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L202-L236
[pal-finalize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L347-L371
[pal-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L2205-L2277
[linux-compute-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6041-L6073
[linux-wrapper]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L199-L349
[linux-commit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L169-L185
[linux-resource-fences]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L1339-L1360
[linux-ring-reserve]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L73-L107
[linux-ring-capacity]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L332-L349
[linux-credits]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L2293-L2331
[linux-compute-ring]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L7125-L7129
[linux-compute-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6076-L6106
[drm-userq-create]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L355-L414
[drm-userq-eop]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L458-L465
[linux-userq-wptr]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_userqueue.c#L35-L89
[linux-mes-input]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_userqueue.c#L134-L146
[mesa-userq-memory]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_userq.c#L29-L80
[mesa-userq-create]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_userq.c#L201-L262
[linux-userq-doorbell]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_userq.c#L467-L524
[mesa-userq-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1636-L1751
[mesa-userq-body]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1516-L1607
[mesa-fence-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L216-L242
[linux-userq-destroy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_userq.c#L537-L580
[mesa-protected-fence]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1571-L1607
[linux-userq-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_userqueue.c#L340-L351
[mesa-fence-history]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1781-L1815
[linux-userq-gfx11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L1649-L1676
[linux-userq-gfx12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L1435-L1449
[linux-userq-mask]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_userq.c#L39-L49
[mesa-userq-signal-abi]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/include/drm-uapi/amdgpu_drm.h#L459-L502
[linux-userq-signal-abi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L468-L507
[pal-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2244-L2306
[pal-idle]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L470-L479
[pal-reuse]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L703-L739
