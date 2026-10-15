# Native compute queue contexts

A compute queue's memory queue descriptor (MQD) connects its command ring to
native execution state: address space, pointer storage, doorbell, scheduling
controls and save backing. The native driver builds this image; a direct HQD
loader or a firmware scheduler installs it on hardware. The process continues
to own its ring and workload storage. Creating, saving or removing this context
is a different transition from completing the work described by its packets.
[Direct creation][create-queue-nocpsch] · [Scheduled creation][create-queue-cpsch]

## Applicability and owners

This chapter describes Linux KFD's ordinary compute queues at the
[source-map revisions](../sources.md). `KFD_IOC_QUEUE_TYPE_COMPUTE` selects PM4
format and `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL` selects AQL format within the same
native compute queue type. The ring producer follows the corresponding
[PM4](pm4/publication.md) or [AQL](aql/publication.md) publication protocol.
DRM scheduled submissions, DRM user queues and Windows contexts have their own
[native owners](architectures.md#native-queue-ownership).
[Format selection][native-input]

| Actor | Information and responsibility |
| --- | --- |
| Process/runtime | Supplies ring address and byte size, pointer addresses, EOP and context-save backing, format, priority and queue percentage. Retains allocations and completes their independent users. |
| KFD process/device queue manager | Acquires mapped resources, creates queue/doorbell identity, chooses the family MQD builder and direct or scheduled path, and owns configuration transitions and native removal. |
| Direct HQD loader | Selects an assigned hardware queue, installs its register image, initializes live pointer/fetch state and activates it. |
| CP hardware scheduler or MES | Receives native mapping commands describing the queue and its process; this path does not call the direct loader during ordinary scheduled creation. |

[Native arguments][create-layout] · [Mapping acquisition][queue-buffers] ·
[Process owner][pqm-create] · [Direct/scheduled selection][device-queue-manager-init]

The family name is the selected implementation, not a claim that every numeric
IP in a range is admitted. KFD's device probe has explicit physical-IP cases;
its DQM selector separately chooses the manager. HIQ and DIQ are distinct
native queue classes, with different initialization callbacks.
[Device admission][kgd2kfd-probe] · [Manager selection][device-queue-manager-init]

| MQD family | Ordinary representation | Direct consumer when selected |
| --- | --- | --- |
| CIK / GFX7 | `cik_mqd`, 128 DWORDs | GFX7 `kgd_hqd_load`. |
| VI / GFX8 | `vi_mqd`, 512 DWORDs | GFX8 `kgd_hqd_load`, with its Tonga EOP-register exception. |
| V9 | `v9_mqd`, 512 DWORDs | Generic GFX9 loader, also selected by Arcturus/Aldebaran tables; GC9.4.3/9.4.4/9.5.0 instead select their specialized loader and MQD wrappers. |
| V10 | `v10_compute_mqd`, 512 DWORDs | Separate GFX10.1 and GFX10.3 loader implementations. |
| V11 | `v11_compute_mqd`, 512 DWORDs | GFX11 loader. |
| V12 / GC12.0 | `v12_compute_mqd`, 512 DWORDs | The native callback table supplies no `hqd_load`, `hqd_destroy` or `hqd_is_occupied`; discovery enables MES. |
| V12.1 / GC12.1.0 | `v12_1_compute_mqd`, 1024 DWORDs | Likewise MES; per-XCC MQD wrappers remain part of construction. |

[CIK manager][cik-manager] · [VI manager][vi-manager] ·
[V9 selection][v9-mqd-manager-init-v9] · [Native GFX9 families][native-select9] ·
[Arcturus][arcturus-callbacks] · [Aldebaran][aldebaran-callbacks] ·
[GFX10][native10-callbacks] · [GFX10.3][native103-callbacks] ·
[GFX11][native11-callbacks] · [GC12 callbacks][native12] ·
[GC12.1 callbacks][native121] · [MES discovery][discovery]

## Process storage and native admission

`kfd_ioctl_create_queue_args` contains 64-bit byte addresses for ring, WPTR,
RPTR, EOP and context backing. Ring size and metadata size are 32-bit byte
counts; EOP size is a 64-bit byte count. Context and control-stack sizes are
32-bit byte counts. The returned doorbell offset describes a native mapping;
it is not the value subsequently written to the doorbell.
[Create representation][create-layout] · [Returned mapping][create-ioctl]

Creation checks a power-of-two requested ring size or zero, then clamps it to
at least 1024 bytes. This input rule does not by itself establish a valid
allocation. Native buffer acquisition requires the ring and pointer storage
to resolve through BO mappings in the process VM. A supplied address passing
`access_ok` is only an earlier check.
[Input checks][native-input] · [BO range and retention][queue-buffers]

| Resource | Native backing established before queue construction |
| --- | --- |
| Main ring and optional metadata | Page-rounded main-plus-metadata extent in one mapped BO range. The legacy GFX7/GFX8 AQL branch instead expects half the advertised ring size, reflecting its double-map convention. |
| RPTR and WPTR storage | Each address resolves through a page-sized mapped BO range. Their logical index units and publication widths still depend on the queue format and native generation. |
| EOP ring | When its address is nonzero, size must meet the topology's EOP minimum; the corresponding mapped range is retained. |
| Context-save region | Exact topology control-stack size and at least the topology save size, expanded across XCCs and debugger storage. This resource additionally has a registered, mapped `GPU_ALWAYS_MAPPED` SVM path. |

[Buffer acquisition][queue-buffers] · [SVM admission][queue-svm] ·
[Legacy ring mapping](aql/publication.md#ring-placement-and-x86-stores) ·
[Full context-save layout](context-save.md#save-area-representation)

ROCr's AQL caller initializes packet headers to INVALID and zeros the AMD
queue descriptor before passing addresses of its 64-bit read/write dispatch
indices to native creation. The thunk preserves those AQL pointer addresses;
for non-AQL queues it instead supplies pointer members in its own queue
object. It allocates EOP and save backing before the create ioctl. Ring
placement and metadata capability have separate predicates in the
[publication](aql/publication.md) and [metadata](aql/metadata.md) chapters.
[ROCr construction][rocr-create] · [Ring initialization][rocr-ring] ·
[Thunk resources][thunk-resources] · [Thunk creation][thunk-create]

## MQD representation and allocation

The following offsets are **DWORD indices in the MQD structure**, not MMIO
register addresses. Low/high address members are adjacent. The common HQD
window remains recognizable across families, but neither the complete image
size nor the shader-state prefix is interchangeable.
[CIK layout][cik-layout] · [VI layout][vi-layout] ·
[V9 layout][v9_mqd-structure] · [V10 layout][v10_compute_mqd-structure] ·
[V11 layout][v11_compute_mqd-structure] · [V12 layouts][struct12]

| MQD member | CIK | VI | V9–V12.1 |
| --- | --- | --- | --- |
| `header` | 0 | 0 | 0 |
| `compute_pipelinestat_enable` | 11 | 11 | 11 |
| `compute_perfcount_enable` | 12 | 12 | 12 |
| `cp_mqd_base_addr_lo` | 51 | 128 | 128 |
| `cp_hqd_active` | 53 | 130 | 130 |
| `cp_hqd_vmid` | 54 | 131 | 131 |
| `cp_hqd_persistent_state` | 55 | 132 | 132 |
| `cp_hqd_pipe_priority` | 56 | 133 | 133 |
| `cp_hqd_queue_priority` | 57 | 134 | 134 |
| `cp_hqd_quantum` | 58 | 135 | 135 |
| `cp_hqd_pq_base_lo` | 59 | 136 | 136 |
| `cp_hqd_pq_rptr` | 61 | 138 | 138 |
| `cp_hqd_pq_rptr_report_addr_lo` | 62 | 139 | 139 |
| `cp_hqd_pq_wptr_poll_addr_lo` | 64 | 141 | 141 |
| `cp_hqd_pq_doorbell_control` | 66 | 143 | 143 |
| `cp_hqd_pq_wptr` | 67 | 144 | absent |
| `cp_hqd_pq_control` | 68 | 145 | 145 |
| `cp_hqd_ib_control` | 72 | 149 | 149 |
| `cp_hqd_iq_timer` | 73 | 150 | 150 |
| `cp_hqd_iq_rptr` | 74 | 151 | 151 |
| `cp_mqd_control` | 85 | 162 | 162 |
| `cp_hqd_eop_base_addr_lo` | absent | 165 | 165 |
| `cp_hqd_eop_control` | absent | 167 | 167 |
| `cp_hqd_ctx_save_base_addr_lo` | absent | 171 | 171 |
| `cp_hqd_aql_control` | absent | absent | 181 |
| `cp_hqd_pq_wptr_lo` | absent | absent | 182 |
| `cp_hqd_pq_wptr_hi` | absent | absent | 183 |

V9's SE4–7 words 39–42 alias its multi-XCC execution fields. V11/V12 instead
place SE4–7 at 44–47. V12.1 places SE0–8 at 49–57 and has separate execution
fields at 40–43. Its metadata-ring base/control occupy 195–197. MQD stride and
PM4 target are at 226 and 225 in the specialized V9 and V12.1 structures.
[V9 union][v9_mqd-structure] · [V11 prefix][v11_compute_mqd-structure] ·
[V12 prefixes and metadata][struct12]

Allocation extent and structure size are separate:

| Native allocation | Extent and owner |
| --- | --- |
| CIK, VI and V10 ordinary queues | GTT suballocation of the selected structure size. |
| V9 compute with CWSR | Dedicated native BO containing a GPU-page-rounded MQD followed by a GPU-page-rounded control stack; the combined slice is host-page-rounded and multiplied by XCC count. The MQD and adjacent control stack use the special GFX9 MQD allocation flag. |
| V9 outside that branch | One structure-sized GTT suballocation. The specialized wrapper still traverses XCC slices; this allocator branch alone does not establish a complete multi-XCC/no-CWSR backing contract. |
| V11 and V12 | `AMDGPU_MQD_SIZE_ALIGN(2048)` = 4096 bytes. |
| V12.1 compute | `NUM_XCC * AMDGPU_MQD_SIZE_ALIGN(4096)` = `NUM_XCC * 8192` bytes. |

[Legacy allocators][cik-init] · [VI allocator][vi-init] ·
[V9 allocation/stride][v9-alloc] · [V10 allocation][v10-allocate-mqd] ·
[V11 allocation][v11-allocate-mqd] · [V12 allocation][v12-init] ·
[V12.1 allocation][v121-init]

`AMDGPU_MQD_SIZE_ALIGN` adds 32 bytes before rounding to a 4096-byte GPU
page. Its source comment reserves a MES fence beyond the structure. It is an
allocation-size calculation: the GTT suballocator uses 512-byte chunks, so a
4096-byte request alone does not imply a 4096-byte start alignment. V9's
dedicated BO placement is also selected separately: its explicit GC9.4.2,
9.4.3, 9.4.4 and 9.5.0 VRAM cases yield to `apu_prefer_gtt`.
[Padding][padding] · [Suballocator][kfd-gtt-sa-allocate] ·
[Pool chunk size][gtt-backing-create] · [V9 placement][vram]

## Initial fields and update rules

Fresh construction clears the image, then writes `header = 0xC0310800` and
`COMPUTE_PIPELINESTAT_ENABLE = 1`. V11/V12/V12.1 clear the padded extent;
earlier families clear the structure. Fields left zero on this path are not
necessarily zero after execution or checkpoint restore.
[CIK init][cik-init] · [VI init][vi-init] · [V9 init][v9-init-mqd] ·
[V10 init][v10-init-mqd] · [V11 init][v11-init-mqd] ·
[V12 init][v12-init] · [V12.1 init][v121-init]

| Control | Emitted initial/update value and applicability |
| --- | --- |
| `CP_HQD_PERSISTENT_STATE` | `PRELOAD_REQ` bit 0 = 1. `PRELOAD_SIZE` bits 17:8 = `0x33` CIK, `0x53` VI/V9/V10, `0x55` V11/V12, `0x63` V12.1. These literals are source policy, without an inferred byte or time unit. |
| `CP_MQD_CONTROL` | `PRIV_STATE` bit 8 = 1; VI also selects MTYPE_UC. This controls the MQD path. The separate HIQ/DIQ initializers add PQ `PRIV_STATE` and `KMD_QUEUE`; those bits do not belong to ordinary application queues. |
| `CP_HQD_QUANTUM` | Enable bit 0 = 1, scale bit 4 = 1, duration bits 13:8 = 10 on CIK and 1 on the later builders. These fields do not guarantee a measured service interval. |
| `CP_HQD_PIPE_PRIORITY` | Native priority 0–6 maps LOW, 7–10 MEDIUM, 11–15 HIGH. `cp_hqd_queue_priority` remains initially zero. The full priority and CU-mask contracts are in [scheduling](scheduling.md). |
| `CP_HQD_PQ_CONTROL` | `RPTR_BLOCK_SIZE` bits 13:8 = 5. V9 and later also set `UNORD_DISPATCH` bit 28. CIK additionally selects `MIN_AVAIL_SIZE` = 3. Ring-size and AQL fields are described below. |
| `CP_HQD_IB_CONTROL` | `MIN_IB_AVAIL_SIZE` bits 21:20 = 3 through V12, 1 on V12.1. V9 additionally sets `IB_EXE_DISABLE` bit 23. VI selects UC/ATC=0; its bit 23 is named `IB_ATC`, with different meaning from V9. |
| `CP_HQD_IQ_TIMER` | VI installs UC/ATC=0; V9–V12.1 updates write zero. |
| `CP_HQD_EOP_RPTR` | VI initializes `INIT_FETCHER` bit 31; modern direct loaders set it when installing live state. |
| Status/scheduler word | V9–V12.1 write literal bit 14 of DWORD 160 (`cp_hqd_hq_scheduler0` in V10, otherwise `cp_hqd_hq_status0`); source comments associate it with debugger DISPATCH_PTR setup. The included masks do not uniformly name that bit. |
| Platform atomics acknowledgment | V11/V12/V12.1 additionally set DWORD 160 bit 29 when `amdgpu_amdkfd_have_atomics_support` returns true. GC12.1 names this bit `PLATFORM_ATOMICS_SUPPORTED`; the older included views do not. |

[Legacy constants][cik-registers] · [VI controls][vi-update] ·
[V9 controls][v9-update-mqd] · [V10 controls][v10-update-mqd] ·
[V11 controls][v11-update-mqd] · [V12 controls][v12-init] ·
[V12.1 controls][v121-update] · [Priority map][priority-map] ·
[HIQ distinction][v9-init-mqd-hiq] · [GC9 masks][gc9_0-hqd-masks] ·
[GC10 masks][gc10_1_0-hqd-masks] · [GC11 masks][gc11_0_0-hqd-masks] ·
[GC12 masks][gc12-masks] · [GC12.1 masks][gc121-masks]

VI's ordinary update installs MTYPE_UC and ATC=0 in the PQ, EOP, IB and IQ
controls. With CWSR enabled and a nonzero save-area address, it also writes
those policies into `CP_HQD_CTX_SAVE_CONTROL`. These are the builder's
memory-type selections; a payload's cache policy and synchronization remain
separate. [VI update][vi-update]

The atomics bit is gated by the platform Boolean, not by the firmware version
mentioned in V11's adjacent comment. KFD separately admits an atomics-less
device only under its native firmware threshold. For GC11 that threshold is
509 with RS64 and zero with F32; the admission predicate treats zero as no
firmware exception. Bit initialization and device admission remain separate
conditions. [Platform helper][amdgpu-amdkfd-have-atomics-support] ·
[Threshold selection][kfd-device-info-init] · [Admission][atomics-admission]

The shader/control prefix has additional owners:

| Fields | Initialization or update |
| --- | --- |
| `compute_static_thread_mgmt_se*` | CIK/VI/V10 initialize SE0–3; V9/V11/V12 initialize SE0–7; V12.1 initializes SE0–8. Default masks are all ones. V11 substitutes `0xffff` for its debug workaround and can update that state separately from a supplied CU mask. Topology mapping and the exact workaround predicate belong to [compute affinity](scheduling.md). |
| `compute_tba_*`, `compute_tma_*`, `compute_pgm_rsrc2` | VI installs 256-byte-unit trap addresses and TRAP_PRESENT when TBA exists. V9 sets TRAP_PRESENT under that condition; V10 and later ordinary initializers do not set it here. [Native trap ownership](pm4/dispatch.md#runtime-trap-and-context-state) supplies the surrounding installation. |
| `compute_perfcount_enable` | VI/V9/V10/V11 initialize from the native profiler process and respond to the exact enable/disable update flags. V12 initializes from that process without those update assignments; V12.1 has neither assignment in these bodies. |
| `compute_resource_limits` | V9 updates FORCE_SIMD_DIST bit 23 for GC ≥ 9.4.2 when update information is present, according to `UPDATE_FLAG_IS_GWS`. [Cooperative queues](cooperative.md) retain the GWS owner. |
| `cp_hqd_ctx_save_*`, `cp_hqd_cntl_stack_*`, `cp_hqd_wg_state_offset` | Enabled CWSR sets QSWITCH_MODE bit 30, context address/size, and initial stack/wave frontiers. VI/V9 also test for a nonzero context address. [Context-save representation](context-save.md#mapping-and-native-queue-state) defines the extents and final users. |

[VI construction][vi-init] · [VI update][vi-update] ·
[V9 construction/update][v9-update-mqd] · [V10 update][v10-update-mqd] ·
[V11 update][v11-update-mqd] · [V12 construction][v12-init] ·
[V12.1 construction][v121-init]

### Ring, address and AQL fields

For a nonzero power-of-two ring of `B` bytes, `CP_HQD_PQ_CONTROL.QUEUE_SIZE`
bits 5:0 contain `log2(B/4)-1`: capacity is `2^(field+1)` DWORDs. CIK/VI/V9
use `order_base_2`; V10 onward use `ffs(B/4)-2`. The native inactive-queue
predicate is separate from this arithmetic; it does not define a zero-length
hardware ring encoding. EOP size has an explicit zero case, otherwise the
same exponent capped at `0xA` (2048 DWORDs, 8192 bytes). The driver's comment
attributes that cap to the per-SE eight-bit EOP done counter. Allocation and
topology checks still apply independently.
[CIK update][cik-update] · [VI update][vi-update] · [V9 update][v9-update-mqd] ·
[V10 update][v10-update-mqd] · [V12.1 update][v121-update]

The builders split MQD self-address, RPTR-report address and WPTR-poll address
as unshifted byte addresses. PQ and EOP bases instead split the byte address
shifted right by eight, giving 256-byte granularity. CIK's ordinary update
does not fill its WPTR-poll-address members; VI and later do. `CP_HQD_VMID`
receives the native queue's VMID; scheduled installation has its own process
address-space owner.

| Address field in the cited GC9–GC12 views | Low/high field bits | GC12.1 change |
| --- | --- | --- |
| MQD self-address and RPTR report | Low 31:2, high 15:0; unshifted bytes. | High expands to 24:0. |
| WPTR poll | Low 31:3, high 15:0; unshifted bytes. | High expands to 24:0. |
| PQ and EOP base | Low 31:0, high 7:0 after the eight-bit shift. | High expands to 16:0. |

These masks describe representable fields, not an independently admitted
virtual-address width. The native mapping and allocation owner supplies valid
addresses. [GC9 masks][gc9_0-hqd-masks] · [GC10 masks][gc10_1_0-hqd-masks] ·
[GC11 masks][gc11_0_0-hqd-masks] · [GC12 masks][gc12-masks] ·
[GC12.1 masks][gc121-masks]

The doorbell offset is a **DWORD BAR index**. Modern
`CP_HQD_PQ_DOORBELL_CONTROL.DOORBELL_OFFSET` occupies bits 27:2; the builder
shifts that index by two. The mapped entry can be eight bytes without making
the input index byte-valued. The direct loader separately sets DOORBELL_EN
bit 30. [Index calculation][doorbell-index] · [Entry width][kfd-device-info-init]

AQL selects additional format state:

| Family | AQL-specific writes |
| --- | --- |
| CIK | `cp_hqd_iq_rptr = 1`; PQ NO_UPDATE_RPTR bit 27. |
| VI | The CIK choices plus SLOT_BASED_WPTR = 2 in PQ bits 26:25. |
| V9 | AQL_CONTROL.CONTROL0 = 1; PQ NO_UPDATE_RPTR = 1, SLOT_BASED_WPTR = 2 in bits 26:25, QUEUE_FULL_EN bit 14 = 1, WPP_CLAMP_EN bit 16 = 1; doorbell BIF_DROP bit 1 = 1. |
| V10–V12.1 | The V9 choices except PQ WPP_CLAMP; SLOT_BASED_WPTR moves to bits 19:18. |

[Legacy format writes][cik-update] · [VI format writes][vi-update] ·
[V9 format writes][v9-update-mqd] · [V10 format writes][v10-update-mqd] ·
[V11 format writes][v11-update-mqd] · [V12.1 format writes][v121-update]

GC10 and later have a different WPP_CLAMP_EN field in persistent-state bit 20;
these initializers do not set it. GC12.1 also adds PQ SCOPE bits 17:16,
IB/MQD SCOPE bits 27:26 and EOP SCOPE bits 20:19, left zero by these builders.
Copying a field position from the earlier PQ register would change another
control. [GC10 fields][gc10_1_0-hqd-masks] · [GC12.1 fields][gc121-masks]

## Installing live state

The direct loaders operate on the assigned HQD under native queue selection.
Their live register writes supplement the initial MQD image:

| Direct loader | Pointer and activation sequence |
| --- | --- |
| GFX7/GFX8 | Load HQD registers; enable doorbell; drop the queue lock around a CPU read of the 32-bit user WPTR; reacquire it; on successful read write `(value << shift) & mask`; activate. The manager supplies shift 4 for AQL, 0 for PM4, mask `ring_bytes/4-1`. A failed read leaves the loaded MQD WPTR. |
| Generic GFX9, GFX10.1, GFX10.3, GFX11 | Load MQD DWORDs 128–183; enable doorbell; seed a 64-bit WPTR from saved RPTR/WPTR; program the user WPTR address and trigger one-shot CP polling; start EOP fetch; activate. The passed shift/mask arguments are unused. |
| GC9.4.3 family | Uses its specialized loader through `CP_HQD_AQL_DISPATCH_ID_HI`, with the same saved-pointer reconstruction and CP poll. Per-XCC wrappers address each dense MQD slice and pass the corresponding hardware XCC ID. |

[GFX7 load][load7] · [GFX8 load][load8] · [Legacy arguments][legacy-load-args] ·
[GFX9 load][native9-kgd-gfx-v9-hqd-load] · [GFX10 load][native10-kgd-hqd-load] ·
[GFX10.3 load][native10_3-hqd-load-v10-3] ·
[GFX11 load][native11-hqd-load-v11] · [Specialized GFX9 load][load9] ·
[Per-XCC load selection][v9-xcc-load]

GFX8 explicitly omits EOP_RPTR, EOP_WPTR and EOP_WPTR_MEM writes on Tonga
because of its documented driver erratum. This is a family-specific load
exception, not a general absence of EOP state. [GFX8 load][load8]

The modern direct-loader reconstruction assumes that the queue has not
overflowed. For DWORD capacity `C`, saved 32-bit RPTR `R`, and saved WPTR
halves `L,H`, the source calculates:

```text
G = R & (C - 1)
if (L & (C - 1)) < G: G += C
G += L & ~(C - 1)
G += uint64(H) << 32
```

It writes `G` before the CP poll obtains the live process pointer. This is
native restoration of an HQD position. It does not change the process's AQL
index unit from packets to DWORDs, nor establish a new producer reservation
algorithm. The same four-bit argument in a wrapper does not imply a CPU
conversion in its selected consumer. [Complete consumer][native9-kgd-gfx-v9-hqd-load]

Scheduled creation follows another path. The V9 CP scheduler's MAP_QUEUES
builder supplies the first MQD address, process WPTR address, doorbell and
native compute selection. GC12 MES instead carries the retained WPTR BO's
MC address plus page offset, `is_aql_queue`, and ring size in DWORDs; the V12
packet calls the latter union member `gds_size` / `kfd_queue_size`. V12.0 and
V12.1 copy different subsets of native process inputs. Their packet builders,
not the unused direct-load wrappers, establish this interface.
[CP mapping][cp-map] · [MES input owner][mes-input] ·
[V12 packet builder][mes12] · [V12.1 packet builder][mes121] ·
[MES representation][mes-api]

## Per-XCC images and metadata

GC9.4.3, GC9.4.4, GC9.5.0 and GC12.1.0 construct one MQD slice per dense
ordinal `i` in the node's XCC count. Each slice has its own MQD address and
byte stride. The same ring, RPTR/WPTR storage, EOP base and doorbell are
installed in all slices; CWSR base advances by `i * context_save_size`.
The [context-save chapter](context-save.md#save-area-representation) defines
that separate user-storage stride.
[V9 wrappers][v9-xcc] · [V12.1 wrappers][v121-xcc]

The initializer takes a rotating logical start and gives AQL slice `i` logical
ID `(start+i) % XCC_count` with `compute_tg_chunk_size = 1`. PM4 instead gets
logical ID 0, chunk size 0 and `pm4_target_xcc_in_xcp` from the native queue
properties. The ioctl obtains that target from queue-percentage bits 15:8,
separate from percentage bits 7:0. Dense storage ordinal, logical execution
ID and a physical set-bit XCC index are different quantities.
[V9 construction][v9-xcc] · [V12.1 construction][v121-xcc] ·
[Native input decoding][native-input] · [Set-bit traversal][v9-xcc-load]

V9's ordinal-zero AQL MQD clears NO_UPDATE_RPTR; its other slices keep it.
V12.1 does not make that override. V9's union also matters at initialization:
generic SE4–7 initialization writes all ones to words 39–42, then the wrapper
overwrites only logical ID at 39 and chunk size at 41. Restart and restore-chunk
words 40 and 42 retain all ones. V12.1's fields do not share that union.
V9's SR-IOV multi-VF branch additionally sets DOORBELL_MODE; its VRAM MQD
branch flushes HDP after construction and update.
[V9 initial and updated slices][v9-xcc] · [V9 union][v9_mqd-structure] ·
[V12.1 separate fields][struct12]

GC12.1 enables its metadata fetcher only when the nonzero metadata ring size
equals four times the main ring size. It derives the metadata base from
`ring_address + ring_size`, shifted by eight; sets KD_FETCHER_ENABLE bit 16
and KD_SIZE bits 1:0 to 2, selecting the source's 64-DWORD metadata record.
Other nonzero ratios warn and receive no metadata assignments. This update
branch does not clear previously installed metadata state. Full paired-slot
publication and lifetime are described in [AQL metadata](aql/metadata.md).
[Metadata builder][v121-update] · [Metadata fields][gc121-metadata]

## Reconfiguration, checkpoint and final ownership

`QUEUE_IS_ACTIVE` means nonzero ring size/address/percentage and neither
evicted nor suspended. KFD updates an active queue by first unmapping it,
removing it from MES, or dequeuing its direct HQD. A reported removal error
stops MQD mutation. It then updates the image and reloads/reschedules the
queue if active. Updating with a null ring address disables eligibility;
it does not establish completion of every dispatched operation.
[Eligibility][queue-properties] · [Complete update][update-queue] ·
[Ring replacement owner][pqm-update]

The direct update path requests SAVE with CWSR enabled and DRAIN otherwise.
The selected callback still determines the native request: generic and
specialized GFX9, GFX10.1 and GFX10.3 explicitly select SAVE_WAVES; GFX7,
GFX8 and GFX11 map SAVE through their default DRAIN_PIPE case. HWS/MES have
separate save/remap protocols. Observing a SAVE enum at the caller is therefore
insufficient to describe a particular direct-loader path.
[GFX7 removal][destroy7] · [GFX8 removal][destroy8] ·
[GFX9 removal][native9-kgd-gfx-v9-hqd-destroy] ·
[GFX10 removal][native10-kgd-hqd-destroy] ·
[GFX10.3 removal][native10_3-hqd-destroy-v10-3] ·
[GFX11 removal][native11-hqd-destroy-v11]

Checkpoint serialization also differs from live save/resume. CIK/VI/V10/V11
copy their structure extent, while V9 can include its separate control stack
and packs multiple MQDs densely in checkpoint output. V11's serialized 2048
bytes are smaller than its 4096-byte allocation. Restore copies saved state,
replaces the doorbell and leaves the queue inactive; it does not rerun fresh
initialization. V9 has a separate MQD-address relocation hook. V12/V12.1
install no checkpoint/restore callbacks in these managers, and requested
restore is rejected by the creation owner. This absence says nothing about
ordinary firmware context switching.
[Legacy copies][legacy-checkpoint] · [VI copies][vi-checkpoint] ·
[V9 serialization][v9-checkpoint] · [V10 copy][v10-checkpoint-mqd] ·
[V11 copy and restore][v11-checkpoint-mqd] ·
[V9 restore and relocation][v9-restore-relocate] ·
[Per-XCC restore][v9-xcc-restore] · [Per-XCC address repair][v9-address] ·
[V12 manager][v12-manager] · [V12.1 manager][v121-manager] ·
[Restore admission][create-queue-cpsch]

A complete process flow is:

1. Establish the mapped ring, pointer, EOP and context resources for the native
   format and topology. Initialize indices and inactive packet state before
   creating the queue; retain all supplied storage.
2. Let the native owner allocate the MQD and install it through the selected
   direct or scheduled path. Publish application commands using that queue's
   format-specific visibility and doorbell protocol.
3. Reconfigure through the native update owner. Keep both old and replacement
   storage live across the transition; releasing a BO-VA queue reference does
   not itself establish the caller's final-use boundary.
4. Complete the workload's independent users and stop new publication before
   ending queue ownership. Normal native destruction removes the queue and
   frees its MQD; the process owner then releases retained queue buffers.
5. Release process ring, indices, EOP and save storage only after the native
   lifecycle has ended. Code, arguments, scratch and shared payloads retain
   their own last-user boundaries, including other queues and saved waves.

[Create and retained mappings][create-ioctl] · [Update owner][pqm-update] ·
[Scheduled destruction][destroy-queue-cpsch] ·
[Direct destruction][direct-destroy] · [MQD free][kfd-free-mqd-cp] ·
[Process retirement][pqm-retirement] · [ROCr teardown][rocr-destroy]

The native destruction code has error cleanup as well as its ordinary success
path: PQM continues cleanup for `-ETIME` and `-EIO`. Neither a freed MQD nor
an internal scheduler status marker proves successful payload execution after
such an error. Ring consumption, native queue retirement and workload
completion remain separate observations.
[Destruction branches][pqm-retirement] ·
[Save and retirement](context-save.md#save-resume-and-final-ownership)

### Scheduler completion and failure diagnostics

Linux's GFX11 MES submission path places two completion observations in one
native command stream: the requested API's completion write and a subsequent
`QUERY_SCHEDULER_STATUS` fence. It reports `MES failed to respond` when the
latter fence times out or the former completion remains absent. That message
identifies the outstanding API, not the first stalled packet or the firmware's
reason for failing it. It does not distinguish an unresponsive command
processor from a scheduler bookkeeping failure. [Submission and checks][mes-completion]

AMD's April 2024 MES specification describes separate circular API, event and
interrupt histories. Their indices identify the next entry to overwrite.
API history contains the operation, start/end timestamps, status and error
code. The documented `REMOVE_QUEUE` error codes are:

| Code | Native classification |
| --- | --- |
| 0 | `API_REMOVEQUEUE_NOERROR` |
| 1 | `API_REMOVEQUEUE_UNMAP_FAIL` |
| 2 | `API_REMOVEQUEUE_HQDQUEUE_MAP_MISMATCH` |
| 3 | `API_REMOVEQUEUE_CLEANUP_FAIL` |
| 4 | `API_REMOVEQUEUE_QUEUE_NOT_FOUND` |
| 5 | `API_REMOVEQUEUE_NULL_GANG` |

For error 1, the corresponding `MES_EVT_LOG_UNMAP_QUEUE` event identifies
the doorbell and command processor's unmap status. Other codes describe
different failures; none identifies a particular application packet without
additional execution state. These firmware records are distinct from the
kernel's timeout message. Their availability depends on the native owner
having supplied and enabled the log buffer. [MES specification, pp. 47–51][mes-diagnostics]

[create-queue-nocpsch]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L736-L860
[create-queue-cpsch]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L2221-L2332
[native-input]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L209-L336
[create-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L73-L93
[queue-buffers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L196-L348
[queue-svm]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L89-L149
[pqm-create]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L324-L516
[device-queue-manager-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L3215-L3352
[kgd2kfd-probe]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L257-L510
[cik-manager]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_cik.c#L383-L464
[vi-manager]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_vi.c#L452-L535
[v9-mqd-manager-init-v9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L1014-L1130
[native-select9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L335-L354
[arcturus-callbacks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_arcturus.c#L392-L424
[aldebaran-callbacks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_aldebaran.c#L166-L197
[native10-callbacks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v10.c#L1093-L1123
[native103-callbacks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v10_3.c#L658-L687
[native11-callbacks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c#L957-L984
[native12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v12.c#L518-L533
[native121]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v12_1.c#L521-L536
[discovery]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2972-L3011
[create-ioctl]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L338-L447
[rocr-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L81-L352
[rocr-ring]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L594-L650
[thunk-resources]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/queues.c#L587-L684
[thunk-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/queues.c#L692-L851
[cik-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/cik_structs.h#L27-L157
[vi-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/vi_structs.h#L159-L417
[v9_mqd-structure]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/v9_structs.h#L159-L690
[v10_compute_mqd-structure]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/v10_structs.h#L675-L1188
[v11_compute_mqd-structure]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/v11_structs.h#L674-L1187
[struct12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/v12_structs.h#L674-L2214
[cik-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_cik.c#L75-L140
[vi-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_vi.c#L78-L160
[v9-alloc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L42-L168
[v10-allocate-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L75-L86
[v11-allocate-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L101-L112
[v12-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L82-L245
[v121-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L140-L322
[padding]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_mes.h#L497-L503
[kfd-gtt-sa-allocate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L1336-L1438
[gtt-backing-create]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L815-L854
[vram]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager.c#L318-L333
[v9-init-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L170-L243
[v10-init-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L88-L153
[v11-init-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L114-L192
[cik-registers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/cik_regs.h#L26-L71
[vi-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_vi.c#L175-L263
[v9-update-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L257-L344
[v10-update-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L169-L239
[v11-update-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L208-L276
[v121-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L239-L322
[priority-map]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager.c#L30-L47
[v9-init-mqd-hiq]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L493-L505
[gc9_0-hqd-masks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_9_0_sh_mask.h#L12794-L13204
[gc10_1_0-hqd-masks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h#L20201-L20627
[gc11_0_0-hqd-masks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_0_0_sh_mask.h#L17261-L17747
[gc12-masks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h#L13161-L13658
[gc121-masks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h#L12925-L13466
[amdgpu-amdkfd-have-atomics-support]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd.c#L776-L779
[kfd-device-info-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L192-L255
[atomics-admission]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L772-L787
[cik-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_cik.c#L172-L221
[doorbell-index]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell_mgr.c#L115-L139
[load7]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v7.c#L159-L202
[load8]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v8.c#L154-L226
[legacy-load-args]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_cik.c#L159-L170
[native9-kgd-gfx-v9-hqd-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v9.c#L222-L299
[native10-kgd-hqd-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v10.c#L208-L288
[native10_3-hqd-load-v10-3]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v10_3.c#L179-L274
[native11-hqd-load-v11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c#L165-L260
[load9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gc_9_4_3.c#L284-L363
[cp-map]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_packet_manager_v9.c#L227-L298
[mes-input]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L207-L282
[mes12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_0.c#L274-L388
[mes121]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L274-L375
[mes-api]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/mes_v12_api_def.h#L338-L430
[v9-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L716-L835
[v121-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L440-L584
[gc121-metadata]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h#L13455-L13466
[queue-properties]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L463-L561
[update-queue]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1070-L1177
[pqm-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L589-L646
[destroy7]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v7.c#L359-L458
[destroy8]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v8.c#L391-L493
[native9-kgd-gfx-v9-hqd-destroy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v9.c#L524-L574
[native10-kgd-hqd-destroy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v10.c#L513-L624
[native10_3-hqd-destroy-v10-3]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v10_3.c#L500-L548
[native11-hqd-destroy-v11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c#L489-L534
[legacy-checkpoint]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_cik.c#L251-L284
[vi-checkpoint]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_vi.c#L295-L329
[v9-checkpoint]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L398-L445
[v10-checkpoint-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L286-L320
[v11-checkpoint-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L322-L353
[v9-address]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L878-L899
[v12-manager]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L383-L467
[v121-manager]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L644-L728
[destroy-queue-cpsch]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L2793-L2896
[kfd-free-mqd-cp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager.c#L226-L235
[pqm-retirement]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L518-L587
[rocr-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L354-L419
[v9-xcc-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L929-L954
[v9-restore-relocate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L447-L491
[v9-xcc-restore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L837-L876
[direct-destroy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L973-L1068
[mes-completion]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v11_0.c#L176-L283
[mes-diagnostics]: https://gpuopen.com/download/documentation/micro_engine_scheduler.pdf#page=47
