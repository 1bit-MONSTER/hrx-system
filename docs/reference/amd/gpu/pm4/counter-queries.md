# PM4 performance-query ownership

RADV implements `VK_KHR_performance_query` by programming PM4 counters around
commands, copying samples into a query allocation, and converting those samples
into public results. The collection has several distinct owners: a host
profiling reference, a GPU submission mutex, query storage, and the native
clock-policy context. Result availability ends none of those lifetimes by
itself. The [counter chapter](counters.md) describes PAL's global-counter
fields and sampling protocol; this chapter follows RADV's query owner.

## Queue and event selection

RADV's extension predicate depends on the implementation revision:

| Mesa revision | Architecture predicate |
| --- | --- |
| `0ba4b08edc65` | `gfx_level == GFX10_3` or `GFX11 <= gfx_level < GFX12`. |
| `44cc4ca677a4` | `GFX10_3 <= gfx_level <= GFX12`. |

Both require RGP tracing to be disabled because SQTT/SPM interferes with the
counter registers. These are RADV predicates, not a hardware counter inventory
for other architectures. The later revision adds GFX12 event selection and
the SQG synchronization sequence below. [Earlier predicate][admission]
[GFX12-inclusive predicate][admission12]

Only `RADV_QUEUE_GENERAL` enumerates counters. That queue supports compute as
well as graphics; the separate compute-only queue family returns a counter
count of zero. The submission path also requires the general queue for a
command buffer using performance counters. [Queue capabilities][queue-types]
[Counter enumeration][enumeration] [Submission selection][submit-selection]

Each exposed counter has `VK_PERFORMANCE_COUNTER_SCOPE_COMMAND_KHR` scope,
`VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR` storage, and the
`VK_PERFORMANCE_COUNTER_DESCRIPTION_CONCURRENTLY_IMPACTED_BIT_KHR` flag.
A counter's UUID identifies RADV's intended semantic quantity, while its
constituent native events depend on the architecture. For example, GFX11
"Waves" uses `SQ` selector `0x14`; Mesa's
GFX11 `SQ` table names the SQG register block. WGP instruction events use a
different block. Command scope specifies the query interval and does not
establish process-local event attribution. [Metadata][enumeration]
[Event definitions and formulas][catalog] [SQG mapping][sqg]

At `44cc4ca677a4`, RADV's GFX12 shader catalog selects these events:

| Counter | Block | Source constant | Event selector |
| --- | --- | --- | --- |
| Waves | `SQ` | `SQ_PERF_SEL_WAVES_GFX12` | `0x13` |
| LDS instructions | `SQ_WGP` | `SQ_PERF_SEL_INSTS_LDS_GFX12` | `0x2d` |
| SALU instructions | `SQ_WGP` | `SQ_PERF_SEL_INSTS_SALU_GFX12` | `0x2e` |
| SMEM loads | `SQ_WGP` | `SQ_PERF_SEL_INSTS_SMEM_GFX12` | `0x2f` |
| VALU instructions | `SQ_WGP` | `SQ_PERF_SEL_INSTS_VALU_GFX12` | `0x32` |
| VMEM loads | `SQ_WGP` | `SQ_PERF_SEL_INSTS_TEX_LOAD_GFX12` | `0x36` |
| VMEM stores | `SQ_WGP` | `SQ_PERF_SEL_INSTS_TEX_STORE_GFX12` | `0x37` |

Its VRAM-read formula weights GL2C 32/64/128/256-byte request events. The
revision also selects TCP miss event `0x11` for GFX11+ and omits the L1
hit-ratio counter on GFX12. A semantic counter name therefore does not select
the same native event on every architecture. [GFX12 event catalog][catalog12]

## Host and GPU owners

Vulkan requires the profiling lock before beginning a command buffer that
uses a performance query. The lock remains held while such a command buffer
is recording, executable, or pending; implementations may admit multiple
holders. A completed reusable command buffer is still executable, so waiting
for its submission does not by itself permit releasing the lock.
[Profiling-lock contract][vk-lock]

RADV implements host acquisition with a device-local reference count. The
first reference requests a DRM stable pstate through an initialized hardware
context; the last release requests `NONE`. The default profiling policy is
`peak`, selected through `RADV_PROFILE_PSTATE`. This host reference neither
serializes GPU commands nor owns another profiler's selectors.
[Host acquire/release][host-owner] [Policy selection][pstate-default]
[Policy names][pstate-names]

The GPU serialization state occupies a separate GTT buffer marked
`RADEON_FLAG_NO_INTERPROCESS_SHARING`. Its layout is a RADV implementation
detail:

| Byte offset | Use |
| --- | --- |
| 0 | Submission mutex; a 32-bit compare-and-swap acquires 0 → 1, and a confirmed 64-bit immediate copy releases it to zero. |
| 8 | Query-end EOP marker; query begin clears it, and query end writes and waits for 1. |
| `16 + 8p` | Eight-byte predicate selecting pass `p`; the allocation reserves 512 predicates. |

[State allocation][state-allocation] [Offsets and capacity][state-layout]
[Mutex and pass writes][gpu-owner] [Query marker][begin-end]

For each counter-using submission, RADV installs a lock command stream in
both the initial and continuation preambles and an unlock command stream in
the postambles. Acquisition clears the default-pass predicate and enables the
submitted pass; release restores pass zero before clearing the mutex. The
winsys appends postamble IBs to the submitted IB list. This is coordination
among users of one logical device's private state, not exclusion against
other Vulkan devices, KFD collectors, or PAL experiments.
[Submission assembly][submit-selection] [Postamble submission][postambles]

The DRM clock owner has its own lifetime. In the pinned Linux implementation,
requesting `NONE` changes the operating policy but does not clear
`stable_pstate_ctx`. Final context release attempts restoration of the saved
policy and clears that owner when access to the live device is admitted. The
complete mechanism and error semantics are in
[native clock ownership](../observability.md#drm-stable-pstate-lifetime).

## Query execution and completed use

Query begin adds the result and private-state buffers to the command stream's
buffer list, clears the EOP marker, waits for preceding work, resets the
counter master, enables SQG event generation, and programs shader and event
selection. RADV selects all shader stages (`0x7f`). Pass predicates guard the
relevant programming and sample-copy commands. It takes beginning samples,
starts the global master, and enables windowed counting.
[Begin sequence][begin-end]

Query end emits a bottom-of-pipe EOP marker and waits for it before sampling.
The sampling helper emits `PERFCOUNTER_SAMPLE`, performs its idle sequence,
disables windowed counting, sets master stop/sample, and copies the selected
values. It then writes the pass's availability marker. Only afterward does
the end helper reset the master and disable the SQG event state. The
clock-gating helper emits register writes on GFX10.3 and returns without a
write on GFX11; native clock policy remains a separate prerequisite.
[Sampling order][sampling] [End sequence][begin-end]
[Clock-gating and event helpers][clock-helpers]

RADV's helper stops the global master for both beginning and ending samples.
PAL's [GFX10 SQ workaround](counters.md#generation-dependent-stop-behavior)
instead uses reset for the beginning sample and keeps counting for the ending
sample. The runtime-selected protocols remain distinct; one caller's ordinary
sequence does not erase another caller's architecture-specific workaround.

The sample copies use confirmed, 64-bit `COPY_DATA` from the performance
register source to TC/L2 memory. The instance loop visits the block's scoped
instances and, for a per-SE block, `max_se` shader engines. This loop and its
broadcast register writes are RADV's selected population, not a portable
physical CU or WGP enumeration. [Instance selection][instances]
[Register indexing][instance-index] [Readback and idle sequence][readback]

After query end, RADV records pending query cache operations. Command-buffer
finalization emits those operations and waits for CP DMA. The submission's
unlock postamble also remains after the query samples. Consequently, a query
availability marker is earlier than both full command retirement and mutex
release. A completed submission fence covers that remaining command stream;
a result accessor supplies no replacement for that ownership boundary.
[Pending query operations][query-flush] [Command-buffer finalization][finalize]
[Unlock placement][submit-selection]

### GFX12 sample transaction ordering

PAL's GFX12 implementation describes a GRBM/SQG ordering hazard: a counter
read can overtake the write that requests sampling and return an older value.
Its `WriteSqSync` toggles `SQG_PERFCOUNTER_CTRL.DISABLE_ME1PIPE3_PERF` after
the sample/stop write and polls for the written value before reading counters.
The register is byte address `0x36760`, DWORD address `0xd9d8`; the field is
bit 19 (`0x00080000`). PAL applies the synchronization to shader engines with
selected SQG or WGP counters. Its filter setup initializes the bit opposite
to the next sample's value, including when beginning sampling is disabled.
[Hazard and poll][pal12-sq-sync] [Sample-before-read ordering][pal12-sample]
[SQG population][pal12-sq-population] [WGP population][pal12-wgp-population]
[Initial toggle value][pal12-filter] [Register address][pal12-sq-address]
[Field mask][pal12-sq-mask]

RADV `44cc4ca677a4` uses the following sequence for exact `gfx_level == GFX12`:

1. Emit `PERFCOUNTER_SAMPLE`, perform the counter idle/cache sequence, disable
   windowed counting, and write global stop with `PERFMON_SAMPLE_ENABLE`.
2. For each shader engine in `[0, max_se)`, select that SE with instance
   broadcast and write `SQG_PERFCOUNTER_CTRL`. The all-stage value is
   `0x0008007f` for beginning samples and `0x0000007f` for ending samples.
3. Emit register-space `WAIT_REG_MEM` for full-DWORD equality with that value,
   mask `0xffffffff`, and native poll-interval field `4`.
4. Restore broadcast instance selection, then execute the pass-selected
   counter copies. Beginning-query setup restarts counting after its samples;
   query end writes availability and subsequently resets counter state.

[Sample master controls][sample-controls12] [SQG write and wait][sq-sync12]
[Placement before copies][sampling12] [Begin/end composition][begin-end12]

The two consumers agree on toggling and observing the SQG control before
counter readback. Their instance populations and shader masks remain their
own policies: RADV visits `max_se`, while PAL tracks the SEs with selected
counters. This register transaction boundary supplements the workload idle,
payload visibility and completed-submission boundaries; none substitutes for
the others.

## Result representation

The query pool deduplicates and sorts the native `(block, selector)` inputs
needed by the requested formulas. For each selector with `N` sampled
instances, it reserves `16N` bytes: interleaved eight-byte beginning and ending
samples for each instance. If there are `P` passes, another `8P` bytes hold
availability markers at the end of each query record. The end builder writes
the low DWORD of a pass marker; query reset zeros its backing first.
[Request expansion][request-expansion] [Pool layout][pool-layout]
[Availability write][sampling]
[Reset implementation][reset]

The host result contains one `VkPerformanceCounterResultKHR` per requested
counter in request order, rather than exposing this raw sample layout. RADV
always fills `float64`. `SUM` subtracts each unsigned 64-bit beginning sample
from its ending sample before summing instances. `MAX` takes the largest
ending sample without subtracting the beginning sample. Weighted sums and
ratios retain their own formulas. Neither a 64-bit copy nor a floating-point
return widens the physical counter or recovers unobserved wraps.
[Reducers][reducers] [Public result order and type][vk-results]

Several source distinctions matter when interpreting the result:

| Quantity | What the pinned sources establish |
| --- | --- |
| GFX11 WGP slots and width | PAL allocates global select indices 0, 2, …, 14 with 32-bit results. Mesa's `SQ_WGP` table advertises 16 sequential select entries, and RADV's generic read path copies 64 bits with an eight-byte register stride. These are different programming descriptions; RADV's storage size does not resolve the physical-width or slot-mapping disagreement. |
| Cache-hit units | RADV labels L0/L1/L2 hit-ratio descriptions `BYTES`, while its reducer computes `100 × (requests − misses) / requests`. The metadata and formula disagree on units. |
| Empty denominators | The ratio reducers have no zero-denominator branch. An empty event population therefore does not acquire a defined zero-utilization interpretation from those formulas. |
| Host readiness | The performance-query branch of `GetQueryPoolResults` inspects availability, optionally waits, then decodes results. It does not translate its unavailable or timed-out state into a new return status. That implementation's return value alone is not a completed-submission observation. |

[PAL WGP allocation][pal-wgp] [Mesa WGP inventory][wgp]
[RADV register copies][readback] [Ratio metadata][ratio-units]
[Arithmetic][reducers] [Host read][host-read]

Performance-query results use their advertised storage type. Vulkan excludes
`VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`, `VK_QUERY_RESULT_PARTIAL_BIT`, and
`VK_QUERY_RESULT_64_BIT` for this query type. Waiting for the submitted
passes before reading also avoids confusing an earlier query use with the
current one. [Result flags][vk-result-flags] [Query reuse][vk-reuse]

## Pass selection and replay

Vulkan's multipass model repeats the same command batch on the same queue,
selecting each `counterPassIndex` at submission. Query reset applies to all
passes. A reset command therefore resides outside the command buffer containing
the corresponding begin query; repeating reset with every pass would erase
earlier results. [Replay contract][vk-results] [Reset restriction][vk-reset]

The pinned RADV pass-count and emission paths contain a concrete mismatch.
For a block with `K` requested native selectors and `C` table-declared counter
slots, pass count uses `ceil(K / C)`; the overall count is the maximum across
blocks. But begin/sample emission advances through that block's selectors by
`p × I`, where `I` is the block's scoped instance count. Slots and instances
are different quantities. GL2C, for example, has four slots, while its instance
count comes from `num_tcc_blocks`. [Pass count][pass-count]
[Selector advancement][begin-end] [GL2C slots][gl2c]
[Instance population][instance-population]

The sample destination additionally advances by `8 × offset × N` bytes,
although the allocated beginning/ending pairs occupy `16 × N` bytes per
selector. Even when `I == C`, those strides differ for a nonzero offset.
These statements describe the producer/layout disagreement in this revision,
not an alternative hardware multiplexing rule. Pass zero has zero selector
offset and does not exercise either discrepancy. [Sample destination][sampling]
[Allocated pairs][pool-layout]

## Complete single-pass caller flow

A compute observation using the SQG wave event illustrates the distinct
completion and cleanup boundaries:

1. Enumerate the selected general queue's counters and retain their UUIDs,
   units, storage types and interference flags. Enable the advertised
   performance-query feature, create a pool for the chosen event, and require
   the requested set to report one pass.
2. Acquire the profiling lock before recording any command buffer using the
   pool. Reset the query with the enabled host-reset operation or a separate
   reset command buffer ordered before the measured submission. Reset occurs
   once for the observation.
3. Record begin query, the compute dispatch, end query, and the workload's own
   output-visibility operations. Submit pass zero and retain the command
   buffer, query pool, executable and workload allocations through completion.
4. Wait for the complete submission, consume the workload output using its
   memory contract, and read the query with the advertised result type.
   Counter collection and output correctness are separate observations.
5. Reset or free the completed command buffers that retain the query, then
   release the profiling lock. Destroy the query pool after its completed use.
   A retained device still owns its private mutex/pass allocation and native
   contexts; destroying that device releases those owners through their
   respective teardown paths.

[Feature exposure][features] [Enumeration][enumeration]
[Vulkan lifetime][vk-lock] [Query reset][vk-reset]
[Result access][host-read] [Query-pool destruction][pool-destroy]
[Device state destruction][state-destroy] [Native context destruction][device-destroy]

This observation includes RADV's idle, sample, cache and serialization commands
and its selected clock policy. Its interval and event population differ from
an uninstrumented dispatch or an independently scheduled SDMA operation.
[Measured intervals](../observability.md#defining-a-measured-interval) retain
those distinctions when comparing counters with timestamps or host latency.

[admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L57-L65
[admission12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_physical_device.c#L55-L64
[catalog12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_perfcounter.c#L227-L380
[sample-controls12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_perfcounter.c#L68-L82
[sq-sync12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_perfcounter.c#L662-L697
[sampling12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_perfcounter.c#L699-L747
[begin-end12]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_perfcounter.c#L754-L847
[pal12-sq-sync]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PerfExperiment.cpp#L3466-L3520
[pal12-sample]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PerfExperiment.cpp#L3150-L3184
[pal12-sq-population]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PerfExperiment.cpp#L652-L663
[pal12-wgp-population]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PerfExperiment.cpp#L531-L540
[pal12-filter]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PerfExperiment.cpp#L2779-L2790
[pal12-sq-address]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_offset.h#L1976-L1977
[pal12-sq-mask]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_mask.h#L6106-L6115
[queue-types]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L3000-L3026
[enumeration]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L834-L886
[catalog]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L113-L271
[sqg]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_perfcounter_gfx11.c#L679-L705
[host-owner]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1952-L2041
[pstate-default]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_instance.c#L271-L281
[pstate-names]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_instance.c#L231-L246
[state-allocation]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L491-L512
[state-layout]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_constants.h#L105-L108
[gpu-owner]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1547-L1605
[submit-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1671-L1716
[postambles]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1124-L1144
[begin-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L663-L757
[sampling]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L610-L660
[clock-helpers]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L587-L631
[instances]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L386-L390
[instance-index]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L486-L507
[readback]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L543-L611
[query-flush]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2663-L2687
[finalize]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L8840-L8903
[request-expansion]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L350-L384
[pool-layout]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L424-L484
[reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1846-L1867
[reducers]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L759-L831
[pal-wgp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L506-L546
[wgp]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_perfcounter_gfx11.c#L781-L814
[ratio-units]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L307-L319
[host-read]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2327-L2344
[pass-count]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L394-L415
[gl2c]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_perfcounter_gfx11.c#L559-L573
[instance-population]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_perfcounter.c#L748-L809
[features]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L1377-L1380
[pool-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1870-L1883
[state-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L514-L529
[device-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1367-L1423
[vk-lock]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/queries.adoc#L2614-L2695
[vk-results]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/queries.adoc#L2543-L2608
[vk-result-flags]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/commonvalidity/query_results_common.adoc#L22-L35
[vk-reuse]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/queries.adoc#L1444-L1468
[vk-reset]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/queries.adoc#L625-L660
