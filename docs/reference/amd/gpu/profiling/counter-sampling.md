# Programmable counter sampling

The command processor can sample hardware counters into memory while the
counting interval continues. The sample commands, participating workload queues,
shared event selectors, and host reader have separate execution and ownership
boundaries. A counter read on a separate observation queue has a different
dependency graph from a read appended after a dispatch on the workload queue.

The [PM4 counter chapter](../pm4/counters.md) describes the event, instance,
slot, width, and PAL experiment protocols. This chapter follows ROCprofiler's
device-counting path and compares it with Tinygrad's dispatch collection.

## Counting state and queue participation

Four controls that appear in these implementations have distinct purposes:

| Control | Meaning in the cited caller |
| --- | --- |
| HSA queue profiling property | Enables firmware-managed dispatch timing. ROCr remaps an already-used queue when changing the property so firmware reads the new value. |
| `COMPUTE_PERFCOUNT_ENABLE` | Enables counting for work submitted through the programmed compute context. AQLProfile exposes a separate enable command, and ROCprofiler injects it into intercepted workload queues. |
| Native PMC ownership with `perfcount_enable` | Requests the native profiler owner and, when selected, counting across queues. Native support, permission, and acquisition success are separate from the PM4 commands. |
| Native profiling power state | Enables counter clocks on the affected targets and changes the device's power policy. This is separate from selector ownership and workload participation. |

[Queue property][queue-property] [Enable command][queue-enable]
[Workload-queue setup][intercept] [Native request][native-owner]

Programming event selectors or enabling counting on the observation queue does
not establish which other queues participate. Conversely, enabling workload
participation does not reserve the device's shared selector state against a
competing collector. Native clock policy and its retained owner are described
in [counter ownership](../observability.md#counter-ownership-and-interpretation).

### Counter clocks and native power state

ROCm documents that RDNA3/Navi3x and RDNA4/Navi4x automatic power mode gates
the perfmon clock for some blocks; `STABLE_STD` enables that clock. This is a
feature-enablement requirement, beyond its effect on measurement stability.
The SDK's warning uses the exact target-version predicate
`110000 <= gfx_target_version < 120500` and checks for `profile_standard`.
That warning is a runtime policy, not a specification that every other target
or power state has equivalent counter behavior. [ROCm requirement][clock-policy]
[Warning predicate][clock-check]

PAL explains the native boundary: GFX11 moved counter-clock control into a
privileged config register, so its PM4 experiment relies on the kernel driver's
clock-mode operation. PAL's Linux implementation retains a DRM context for
`AMDGPU_CTX_OP_SET_STABLE_PSTATE`; its alternate implementation writes sysfs.
The packet builder does not perform either operation.
[Privileged clock control][pal-clock-register]
[PAL native clock mode][pal-clock-mode]

Runtime power transitions also matter. Linux commit `f931c54b241c` restores
the stable pstate after runtime resume when a profile mode remains selected.
The change explains that GFX shutdown can clear the perfmon clock state while
the software profile level remains set. On the affected predecessor path,
reading back a selected profile level alone therefore does not establish
that the hardware clock state survived a suspend/resume cycle.
[Native resume correction][clock-resume]

## ROCprofiler device-counting sequence

The cited SDK creates a dedicated 64-packet AQL queue at normal priority for
device counting. It generates separate start, read, and stop indirect buffers;
these operate on a retained result allocation. The v2 packet-construction path
selects the ordinary PMC factory rather than its optional concurrent template.
[Observation queue][observation-queue] [Packet construction][packet-construction]
[Factory default][factory-default]

For the GFX11 builder and ordinary CPC/SQG counters, the sequence is:

1. Start emits `CS_PARTIAL_FLUSH` on the observation stream, restores
   `GRBM_GFX_INDEX` broadcast, resets the counter master, programs the selected
   events and block controls, enables compute counting, then resets and starts
   the master. A second compute dependency follows start.
2. Read writes `CP_PERFMON_CNTL = 0x401`: `PERFMON_STATE = 1` together with
   `PERFMON_SAMPLE_ENABLE = 1`. This requests a sample while counting remains
   started. It restores broadcast, emits `CS_PARTIAL_FLUSH`, selects each
   requested instance, copies its counter registers, and restores broadcast.
3. The read builder emits `ACQUIRE_MEM` with `GCR_CNTL = 0x18000`, selecting
   forward sequencing and GL2 writeback. The encoded address and size do not
   narrow GL2 writeback because the GL2 range selection is `ALL`.
4. The SDK submits the read vendor packet followed by an AQL barrier with
   SYSTEM acquire/release scopes and a completion signal. Its host sample
   handler decodes the retained result only after that completion.
5. Before another sample can reuse that allocation, the caller waits for the
   preceding handler. The asynchronous API flag changes when the requesting
   host call waits; it does not grant simultaneous reuse of the same result.
6. Stop joins the outstanding handler, submits the stop packet, and waits for
   its completion. The GFX11 stop builder restores broadcast, writes master
   state 2, and emits `CS_PARTIAL_FLUSH`. Reset and per-queue disable remain
   distinct operations.

[Start builder][start] [Master values][master-values] [Read builder][read]
[Sample request and final flush][read-envelope]
[Cache packet][cache] [Cache fields](../pm4/cache.md#acquire-gcr-fields)
[Read and completion][device-read] [AQL scopes][device-scopes]
[Handler and reuse][handler]
[Stop builder][stop] [Device stop][device-stop]

The optional concurrent template removes the two compute dependencies around
start; its read and stop still contain `CS_PARTIAL_FLUSH`. That fact alone
does not imply a join of a resident dispatch on another queue. The complete
device-counting caller submits those commands to the observation queue without
an explicit dependency on that dispatch. Appending the same read behind a
resident dispatch on its own queue changes its ability to progress. Event
population, scheduler placement, and sampling-stream completion remain separate
from one another. [Template selection][factory] [Start builder][start]
[Observation queue][observation-queue] [Device read][device-read]

The SDK requests native PMC ownership and PTL control but does not gate every
start on successful acquisition. A successful SDK start therefore does not
prove exclusive ownership or that all requested native controls took effect.
Those results retain their own meaning. [Start controls][device-controls]
[Native results][native-owner]

## WGP result coordinates and harvested units

A flat counter-result index describes the producer's memory layout, not a
portable hardware unit number. AQLProfile's WGP counter reader places WGP
inside the shader-array and shader-engine loops. For one event across all
block instances on a single XCC, the element index is:

```text
element = (((instance * shader_engine_count + se) * shader_array_count + sa)
           * physical_wgp_span + wgp)
byte_offset = element * 8
```

Each slot occupies two DWORDs, including counters whose result register is only
32 bits; the builder initializes their high DWORD to zero. The dimension
metadata orders `INSTANCE`, `SE`, `SA`, then `WGP`, with the last coordinate
changing fastest. GFX11 retains a singleton `INSTANCE`; GFX12 WGP blocks omit
it when the block has only one instance. Multi-chiplet metadata can prefix
`XCD` or `AID` according to the block's scope. Those outer dimensions do not
turn a per-WGP result into a device aggregate. [Read loops][wgp-read-layout]
[Dimension metadata][wgp-dimensions]

On GFX11, `physical_wgp_span` is the larger of the CU-count-derived initial
extent and the highest active physical WGP index plus one. The reader retains
holes in this extent: it emits no counter-register read for a harvested WGP
and initializes both result DWORDs to zero. For example, active WGPs
`{0, 1, 2, 4}` require five slots per shader array, even though only four slots
receive hardware samples. Computing the result extent from the active CU
count alone loses the last slot. A zero-filled hole is structural metadata,
not evidence that an active unit observed zero events. [Physical extent][wgp-span]
[Hole initialization][wgp-read-layout]

The GFX11 mask uses two CU bits per WGP and the kernel's folded bitmap layout:
logical `(se, sa)` selects `bits[se % 4][sa + (se / 4) * shader_array_count]`.
The cited builder treats an empty or out-of-window bitmap entry as unavailable
metadata and synthesizes a fully active mask. That fallback is a producer
policy; it cannot distinguish missing topology information from an entirely
inactive array. [Bitmap interpretation][wgp-span]

The same source's GFX12 reader retains its CU-count-derived WGP extent rather
than applying the GFX11 mask. When the agent name begins with `gfx1250`, it
skips physical `(sa = 1, wgp = 8)` in every shader engine, leaves the slot in
the layout, and initializes it to zero. The source identifies this as a
workaround for counter reads that can soft-hang. This exact source predicate
does not establish a general GFX12 harvesting rule. The corresponding factory
registers the WGP `SQ` and `TCP` blocks; a block definition present in a header
does not itself establish factory admission. [GFX12 read predicate][wgp-read-layout]
[GFX12.5 block admission][gfx1250-blocks]

## GFX11 register-read protocols

The pinned public producers emit different `COPY_DATA` operands:

| Producer | Source selector | Destination selector | Width and write confirmation |
| --- | --- | --- | --- |
| ROCm GFX11 builder, resolved CPC/SQG result addresses | 0, memory-mapped register | 2, TC/L2 | Separate 32-bit halves; confirmation clear. |
| Tinygrad PMC | 4, performance counter | 2, TC/L2 | 32-bit low and, when present, high register; confirmation clear. |
| PAL compute counter helper | 4, performance counter | 2, TC/L2 | Separate 32-bit halves; confirmation set. |

[ROCm address selection and copies][copy] [Tinygrad copies][tiny-pmc]
[PAL copy helper][pal-copy]

ROCm resolves register offsets through the device's GC segment table before
choosing the source. With segment 1 at DWORD address `0xa000`, CPC slot 0
results resolve to `0xd006/0xd007`, and SQG slot 0 to `0xd1e4/0xd1e5`.
Its GFX11 builder classifies `0x2000..0x9fff` as privileged config and uses
the performance-counter source only for that range. The resolved result
addresses above therefore select ordinary MMIO. Classifying the unbased
`0x3006` register offset would reach the wrong branch.
[CPC offsets][cpc-offsets] [SQG offsets][sqg-offsets]
[GC segment table][segments] [Address resolution][resolution]
[Privileged range][config-range] [Source predicate][source-predicate]

For a symbolic destination `D`, that builder emits this six-DWORD low-half
copy:

```text
0xc0044000  0x00000200  0x0000d006  0  D.low32  D.high32
```

The control word differs from Tinygrad's `0x00000204` and PAL's confirmed
`0x00100204`. Write confirmation concerns the destination transaction; clearing
it does not establish that a source-register access cannot stall. Their
surrounding sampling, cache, and submission-completion sequences remain part
of each protocol. [ROCm copy][copy] [Tinygrad copies][tiny-pmc]
[PAL copy helper][pal-copy]

ROCm's cited GFX11 PMC path and Tinygrad's PMC path emit no
`PERF_COUNTER_WINDOW`. PAL selects that packet by firmware version and pairs
it around register programming. These are distinct producer paths; absence in
one is not a specification permitting removal from another. The
[window description](../pm4/counters.md#programming-windows-and-register-access)
records PAL's invariants and the public field-width disagreement.
[ROCm builders][start] [ROCm read][read] [ROCm stop][stop]
[Tinygrad PMC][tiny-pmc]

## Tinygrad dispatch collection and retained samples

Tinygrad's KFD path requires `profile_standard` for PMC/SQTT on non-GFX9
targets. That requirement is native power state outside the emitted command
stream. Its separate PCI interface owns different device initialization and
does not perform the same KFD request. [KFD profile mode][tiny-clock]
[PCI interface][tiny-pci]

The cited PMC topology map includes GRBM, GL2C, TCC, and SQ. For GFX11+ SQ it
uses even select-register indices and physical WGP activity masks. This is a
WGP SQ path, not the separate SQG block described above. Its dispatch builder
emits `CS_PARTIAL_FLUSH` before sampling, then resets and restarts counters
after copying the sample. Repeated values therefore represent successive
dispatch intervals rather than one uninterrupted cumulative counter epoch.
[Counter setup and read][tiny-pmc] [Dispatch order][tiny-dispatch]
[Physical activity][tiny-activity]

Results occupy a bounded array of slots. The collector runs after timeline
synchronization, reports when newer profiles overwrote older slots, and copies
only the retained suffix into CPU-owned records. This is useful bounded
collection with explicit loss. It supplies neither a live consumer-credit
protocol nor a proof that an individual resident dispatch can be sampled by
that dispatch-local sequence. [Timeline completion][tiny-completion]
[Slot collection][tiny-collection]

[queue-property]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1592-L1608
[queue-enable]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L390-L404
[intercept]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue.cpp#L1379-L1418
[native-owner]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/ioctl.cpp#L100-L212
[clock-policy]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/docs/how-to/using-rocprofv3.rst#L30-L39
[clock-check]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/controller.cpp#L48-L101
[pal-clock-register]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2053-L2066
[pal-clock-mode]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L5130-L5239
[clock-resume]: https://github.com/torvalds/linux/commit/f931c54b241ce2f36bfc34955aec43a188276b8d
[observation-queue]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/agent_cache.cpp#L131-L168
[packet-construction]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/counters.cpp#L300-L363
[factory-default]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/pm4_factory.h#L134-L137
[factory]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/core/gfx11_factory.cpp#L30-L49
[start]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L414-L735
[master-values]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/gfxip/gfx11/gfx11_primitives.h#L297-L338
[read]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L739-L1001
[read-envelope]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L1099-L1210
[cache]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx11_cmd_builder.h#L81-L118
[device-read]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/device_counting.cpp#L295-L400
[device-scopes]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/device_counting.cpp#L124-L132
[handler]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/device_counting.cpp#L179-L244
[stop]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L1005-L1097
[device-stop]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/device_counting.cpp#L571-L635
[device-controls]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/counters/device_counting.cpp#L466-L478
[wgp-span]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L310-L386
[wgp-read-layout]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L862-L976
[wgp-dimensions]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/aqlprofile/core/counter_dimensions.hpp#L90-L169
[gfx1250-blocks]: https://github.com/ROCm/rocm-systems/blob/51ec055f9e83ea5090aea479982400bd30e31b26/projects/rocprofiler-sdk/source/lib/aqlprofile/core/gfx1250_factory.cpp#L108-L147
[copy]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx11_cmd_builder.h#L232-L308
[pal-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L1604-L1626
[cpc-offsets]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/linux/registers/gc/gc_11_0_0_offset.h#L8372-L8379
[sqg-offsets]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/linux/registers/gc/gc_11_0_0_offset.h#L8582-L8585
[segments]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/linux/registers/sienna_cichlid_ip_offset.h#L88-L94
[resolution]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/util/reg_offsets.h#L118-L138
[config-range]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/linux/soc21_enum.h#L20862-L20863
[source-predicate]: https://github.com/ROCm/rocm-systems/blob/eab38980a6f5cc0e4b17ec5437345e7680bbc936/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx11_cmd_builder.h#L58-L61
[tiny-clock]: https://github.com/tinygrad/tinygrad/blob/e8308c08f77148dce3bfd141cbdfb1be66329a45/tinygrad/runtime/ops_amd.py#L693-L702
[tiny-pci]: https://github.com/tinygrad/tinygrad/blob/e8308c08f77148dce3bfd141cbdfb1be66329a45/tinygrad/runtime/ops_amd.py#L711-L727
[tiny-pmc]: https://github.com/tinygrad/tinygrad/blob/e8308c08f77148dce3bfd141cbdfb1be66329a45/tinygrad/runtime/ops_amd.py#L205-L257
[tiny-dispatch]: https://github.com/tinygrad/tinygrad/blob/e8308c08f77148dce3bfd141cbdfb1be66329a45/tinygrad/runtime/ops_amd.py#L399-L414
[tiny-activity]: https://github.com/tinygrad/tinygrad/blob/e8308c08f77148dce3bfd141cbdfb1be66329a45/tinygrad/runtime/ops_amd.py#L704-L709
[tiny-completion]: https://github.com/tinygrad/tinygrad/blob/e8308c08f77148dce3bfd141cbdfb1be66329a45/tinygrad/device.py#L427-L432
[tiny-collection]: https://github.com/tinygrad/tinygrad/blob/e8308c08f77148dce3bfd141cbdfb1be66329a45/tinygrad/runtime/ops_amd.py#L1015-L1030
