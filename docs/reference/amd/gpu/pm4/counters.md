# PM4 performance counters

PM4 can configure hardware event counters, bracket GPU work, and copy sampled
values into memory. The result belongs to a particular event, hardware block,
instance and counting interval. A completed register copy alone does not
establish those meanings or retire the command stream that produced it.

This chapter follows PAL's GFX11 global-counter path and its `GpaSession`
cumulative samples. The programming owner is a graphics or compute command
stream; a DMA counter can observe an SDMA engine even though PM4 programs and
reads it. Streaming performance monitoring, thread traces and pipeline-statistic
queries have different representations. [Counter types][counter-types]
[Actual profiling caller][profiler-begin]

[RADV performance queries](counter-queries.md) compose the same class of native
counters with Vulkan command lifetimes, a private submission mutex, pass
selection and derived results. That path's scope, slot and width differences
remain separate from PAL's global-counter representation below.

## Owner and applicability

PAL discovers performance-experiment properties from the selected device, then
resolves each requested block, instance and event against its architecture
tables. The field's representable event range is not the block's event catalog.
The client also supplies the event's meaning: the public `PerfCounterId`
contract explicitly allows event IDs to change between chips and requires
each desired instance to be requested separately. [Property discovery][discovery]
[Event identity][counter-id] [Request construction][request-builder]

The counter state is shared hardware state. PAL selects
`PERFMON_ENABLE_MODE = ALWAYS_COUNT`; its implementation explicitly does not
use that field's per-context filtering. A shader-stage mask narrows the event
population by stage, not by process or queue. A PAL experiment object owns its
configuration and result allocation, but does not establish exclusion from
other collectors. Native permissions, KFD ownership and competing state owners
are described in [counter ownership](../observability.md#counter-ownership-and-interpretation).
[Master control and context policy][begin-control]

GFX11 also separates programming access from the interval being counted.
PAL surrounds register programming with `PERF_COUNTER_WINDOW` when its
firmware predicate allows it: MEC version at least 2290 for compute, or PFP
version at least 2240 for the graphics path. Below those thresholds the builder
emits a same-sized NOP. The command-stream wrapper emits the window protocol
only on GFX11. Closing that programming window after start does
not stop counting; the stop commands are separate. GFX11 perfmon clock control
belongs to the kernel-driver clock-mode service because the register is
privileged. [Window thresholds][window-versions]
[Firmware comparison][window-comparison] [Window builder][window-builder]
[Generation selection][window-caller] [Start and close][start-counting]
[Clock owner][begin-control]

## Event, instance, slot and width

A global-counter request contains `block`, `instance`, `eventId` and any
block-specific subconfiguration. The assigned hardware slot is a different
coordinate. The finalized result layout returns the block, instance, event,
assigned slot, raw data type, and byte offsets for the beginning and ending
values. Event-specific subconfiguration remains part of the collector's
configuration record.
[Request fields][counter-request] [Result fields][counter-layout]
[Assigned layout][layout-builder]

The GFX11 tables and builders distinguish three useful examples:

| PAL block | Native counter storage and instance | Global slots per instance | Raw width |
| --- | --- | --- | --- |
| `Sq` | `SQG_PERFCOUNTER*`; one local instance per active shader engine | Eight | 64 bits, LO and HI registers |
| `SqWgp` | `SQ_PERFCOUNTER*`; WGP instances within each shader array and shader engine | Eight, using select-register indices 0, 2, …, 14 | 32 bits, one result register |
| `Dma` | `SDMA<n>_PERFCOUNTER*`; native available DMA-engine instances, capped by PAL's table | Two | 64 bits, LO and HI registers |

[SQG and WGP inventory][sq-inventory] [DMA inventory][dma-inventory]
[SQG/WGP allocation][sq-allocation] [Generic counter width][generic-allocation]

For WGP counters, select-register index `2k` maps to result register `k`.
The returned slot retains the select index. PAL also constrains some events
to particular select slots; the nine-bit event field does not imply that
every slot accepts every event. DMA instance selection uses distinct register
addresses rather than treating an engine number as an SQ instance.
[WGP slot mapping][sq-allocation] [DMA read addresses][generic-read]

For these SQG, WGP SQ and DMA examples, the flattened instance coordinate is
resolved as follows, with `I` the requested instance, `L` the number of local
instances in the block's scope, and `A` the number of shader arrays per shader
engine:

| Block distribution | Coordinates |
| --- | --- |
| Global block | Local instance = `I` |
| Per shader engine | SE = `I / L`; local instance = `I % L` |
| Per shader array | SE = `(I / L) / A`; SA = `(I / L) % A`; local instance = `I % L` |

PAL validates the coordinates against native chip properties, maps virtual SE
numbers to real SE numbers, and packs a WGP's above/below-SPI location into
`GRBM_GFX_INDEX`. A flat instance is therefore neither a queue number nor a
portable CU index. [Instance resolution][instance-map]
[Native register selection][grbm-map]

WGP programming has an additional power and virtualization premise. PAL permits
different configurations per WGP only when GFXOFF, virtualization/SR-IOV,
VDDGFX power-down, clock gating and power gating are disabled. Otherwise the
client must give every WGP in an SE identical counter programming. The results
remain per-WGP; identical configuration does not aggregate them into one SE
value. [WGP configuration contract][counter-request]

### Representative selectors and units

The pinned GFX11 tables use different event namespaces for SQG, WGP SQ and
SDMA. Representative raw selectors are:

| Counter block | Event selectors | Quantity identified by the event |
| --- | --- | --- |
| SQG | `SQG_PERF_SEL_WAVES = 0x14`; `WAVES_32 = 0x15`; `WAVES_64 = 0x16` | Wave events at SQG, with separate wave-size selections |
| WGP SQ | `SQ_PERF_SEL_WAVES = 0x04`; `WAVES_32 = 0x05`; `WAVES_64 = 0x06` | Wave events in the selected WGP SQ instance |
| SDMA | `SDMA_PERFMON_SEL_CYCLE = 0x00`; `IDLE = 0x01` | The SDMA cycle and idle events, not bytes transferred |

[SQG selectors][sqg-events] [SQ selectors][sq-events]
[SDMA selectors][dma-events]

A wave event is not a workitem count, and an SDMA cycle count is not a GPU
timestamp. An event name alone does not supply an instruction, byte or time
conversion. Clock gating, event-specific increment rules, enabled stages and
selected instances belong to that interpretation. Summing a value over
instances is an explicit aggregation of the chosen population. PAL's profiler
does this using its instance masks; it does not merge SQG and WGP observations
as if they were disjoint populations. [Instance aggregation][profiler-results]
[Clock domains](../observability.md#clock-domains-and-conversion) describe the
separate inputs needed for elapsed-time conversion.

## GFX11 control representation

These fields are in 32-bit registers, rather than one self-contained counter
packet. The same `CP_PERFMON_CNTL` write controls the global and SPM masters;
the cumulative path below keeps SPM disabled.

| Register | Bits | Meaning in this path |
| --- | --- | --- |
| `CP_PERFMON_CNTL` | 3:0 | Global state: 0 disable/reset, 1 start, 2 stop |
| `CP_PERFMON_CNTL` | 7:4 | SPM state; 0 disable/reset for a global-only experiment |
| `CP_PERFMON_CNTL` | 9:8 | Enable mode; PAL uses 0, always count |
| `CP_PERFMON_CNTL` | 10 | Sample enable |
| `SQG_PERFCOUNTER<n>_SELECT`, `SQ_PERFCOUNTER<n>_SELECT` | 8:0, 23:20, 31:28 | Event selection, SPM mode, counter mode; global collection uses SPM off and accumulation |
| `SDMA<n>_PERFCOUNTER<m>_SELECT` | 9:0, 23:20, 31:28 | Event selection, counter mode, accumulation mode for the selected global module |
| `SQG_PERFCOUNTER_CTRL`, `SQ_PERFCOUNTER_CTRL` | 0, 2, 4, 6 | PS, GS, HS and CS stage enables; GFX11 has separate controls for SQG and WGP SQ |
| `COMPUTE_PERFCOUNT_ENABLE` | 0 | Start/stop windowed counting on the compute path |

[Master fields][master-fields] [Master values][master-values]
[SQG select fields][sqg-fields] [SQ select fields][sq-fields]
[SDMA select fields][dma-fields] [SQG stage fields][sqg-stage-fields]
[SQ stage fields][sq-stage-fields] [Compute enable][compute-enable]

PAL programs accumulation mode and disables streaming mode in each allocated
module. A global counter consumes the whole generic module. It writes the
shader masks after `CP_PERFMON_CNTL`, because the CP associates ownership of
that state with the master control. The default mask includes all shader
stages; a compute-only observation supplies the CS mask explicitly.
[Module allocation][generic-allocation] [Shader controls][shader-controls]
[Mask input][sample-config]

## Reset, start, sample and stop

For a global-only GFX11 experiment, PAL's ordinary command sequence is:

1. Open the supported programming window and join preceding work with the
   selected idle/cache operation. Disable and reset the global and SPM
   masters. Enable the required SQG events, set shader masks, and write the
   selected instance and event registers.
2. Take the beginning sample. The actual builder emits `PERFCOUNTER_SAMPLE`,
   waits for compute work, selects the broadcast register context, and disables
   windowed counting. It sets global sample-enable together with global stop,
   then copies each selected value into the beginning region.
3. Start the global master and enable windowed counting. On a graphics-capable
   engine, PAL emits `PERFCOUNTER_START`; it also writes
   `COMPUTE_PERFCOUNT_ENABLE`. Close the programming window and execute the
   measured commands.
4. Reopen the programming window and join the measured work. Run the same
   sample/stop/read sequence into the ending region. On the compute path,
   windowed stop clears `COMPUTE_PERFCOUNT_ENABLE`; graphics has a corresponding
   stop event, subject to its named workaround.
5. Disable and reset both masters, turn off the SQG event state owned by the
   experiment, and close the programming window. This cleanup establishes
   PAL's inactive state; it does not reconstruct another collector's prior
   selectors or experiment.

[Begin/reset/setup][begin-control] [Shader controls][shader-controls]
[Selector writes and beginning sample][begin-sample]
[Actual sample order][sample-stop] [Start][start-counting]
[Windowed enable and disable][windowed-control] [End and cleanup][end-control]

The idle operation has a precise scope. With cache flushing requested, PAL
uses a waited EOP and the corresponding cache writeback/invalidation. Without
it, PAL waits for CS work, and on graphics also stalls the relevant graphics
surfaces. The source explicitly says this cheaper path is not a full wait
for the bottom of every graphics/event pipeline. Neither path by itself
orders work on an independent SDMA queue. A DMA observation needs a start
dependency before that engine's measured work and its completed-use dependency
before the ending sample. The DMA block's cycle events describe that engine,
not CP DMA_DATA traffic. [Idle implementation][idle-control]
[DMA block ownership][dma-inventory]

PAL reads a 32-bit counter register using confirmed `COPY_DATA` from the
performance-counter source to the TC/L2 memory destination, with four-byte
destination alignment. A 64-bit sample is two such copies, low then high.
This relies on the sampling/stop protocol; it is not an atomic live 64-bit
register read. The resulting memory still needs the completion and visibility
steps below before CPU consumption. [Read primitive][copy-counter]
[Two-DWORD sample][copy64] [WGP sample][wgp-read]

### Generation-dependent stop behavior

The GFX10 workaround setup explicitly enables `waNeverStopSqCounters` because
stopping SQ counters can leave them stuck. When PAL's experiment selects that
workaround, beginning samples use disable/reset and ending samples keep the
global master running while sampling. This differs from the ordinary stopped
GFX11 sequence above. The selector and state builders do not establish a
universal latch or simultaneous-sample guarantee across all blocks; a live
counter cannot be treated as frozen merely because a sample command occurred.
[Workaround predicate][stop-workaround] [Experiment selection][layout-finalize]
[Alternative sampling states][sample-stop]

GFX12 has a separate sample-transaction ordering requirement. PAL and RADV
toggle and poll `SQG_PERFCOUNTER_CTRL.DISABLE_ME1PIPE3_PERF` before reading
counters so that the read follows the sampling write. The exact register,
phase values and consumer instance policies are in
[GFX12 sample transaction ordering](counter-queries.md#gfx12-sample-transaction-ordering).

## A complete cumulative-sample owner

A representative shader observation requests the SQG wave event for each
desired active SE, with the CS shader mask. The selected device supplies
instance counts, available modules and event limits. `GpaSession` records
the ordered `(block, instance, event)` list and owns the experiment objects
and backing allocations. Its cumulative sample must begin and end in the
same command buffer; the public contract calls out interference from other
applications when a cumulative interval spans command buffers.
[Session owner and restriction][session-contract] [Request construction][request-builder]
[Same-buffer rule][sample-end-contract]

The real PAL profiler composes the mechanism as follows:

1. Acquire a session, call `Begin`, and record `BeginSample` into a graphics
   or compute target command buffer. `GpaSession` creates and finalizes the
   counter experiment, queries its memory requirements, allocates cacheable
   GART result storage, binds it, and emits `CmdBeginPerfExperiment` only
   after the sample's resources have been initialized.
2. Record the measured workload, `EndSample`, then session `End` in the last
   submitted command buffer belonging to the session. `EndSample` emits the
   native counter stop/read/cleanup. Session `End` adds a CP-to-memory
   visibility barrier, a bottom-of-pipe completion event, and another barrier
   for event visibility.
3. Submit the complete command buffer and retain the session, command buffers
   and GPU allocations. PAL's profiler associates a queue-owned fence with
   the submission. `ProcessIdleSubmits` processes a pending submission only
   after that fence reports success, before consuming logged results or
   recycling those owners.
4. Read each result using the finalized layout, then reset or destroy the
   session only after completed use. The profiler outputs results first,
   resets the completed command buffers, and then recycles or deletes the
   corresponding sessions.

[Caller begin and end][profiler-session] [Sample begin][profiler-begin]
[Allocation and binding][result-memory] [Emit after initialization][sample-initialize]
[Sample end][sample-end] [Session visibility and event][session-end]
[Submission fence][profiler-submit] [Completed-owner retirement][profiler-retire]

The public session interface also exposes `IsReady` for result readiness.
Its implementation checks the session event and, when queue timing is enabled,
the corresponding timed-queue fences. `GetResults` is a result accessor, not
an implicit wait. The complete-submission fence used by the profiler also
covers commands after the event, including its final visibility barrier.
Result readiness, result copying and command-storage retirement remain
separate observations. [Readiness][session-ready] [Result interface][result-contract]
[Reuse prerequisite][reset-contract]

## Result interpretation and overflow

For global counters, PAL packs width-sized samples consecutively into a
beginning region and an ending region. The returned byte offsets and data
types determine where and how to read them; a mixed 32/64-bit list is not an
array of raw 64-bit counters. [Layout construction][layout-finalize]
[Layout query][layout-builder]

`GpaSession` returns an array of 64-bit values in request order. It loads
each raw beginning and ending sample using the reported width, widens them
to `uint64`, then subtracts `end - begin`. For a 32-bit counter this code
does not mask the subtraction back to 32 bits or report lost wraps. A wider
return type does not widen the physical counter. Even modular subtraction
would require a bound on the number of wraps; these result records contain
no wrap count. [Raw-width loads and subtraction][result-decode]

The interpretation therefore keeps the event definition, raw width, enabled
stages, instance set, sampling mode and collection interval together. A sum,
rate or utilization ratio adds assumptions about those inputs. The source
does not make a DMA idle event a bandwidth counter, nor does an always-count
SQG experiment become per-process merely because one command buffer owns its
result storage. [Event identity][counter-id] [Context policy][begin-control]
[Aggregation](../observability.md#event-identity-and-aggregation) and
[measurement intervals](../observability.md#defining-a-measured-interval)
describe those additional relationships.

[counter-types]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palPerfExperiment.h#L106-L129
[discovery]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L630-L640
[counter-id]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/gpuUtil/palGpaSession.h#L114-L129
[counter-request]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palPerfExperiment.h#L181-L220
[counter-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palPerfExperiment.h#L233-L250
[request-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L3930-L3985
[sq-inventory]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfCtrInfo.cpp#L1440-L1498
[dma-inventory]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfCtrInfo.cpp#L1672-L1693
[sq-allocation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L462-L546
[generic-allocation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L660-L695
[instance-map]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2554-L2617
[grbm-map]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2622-L2693
[sqg-events]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_enum.h#L10864-L10879
[sq-events]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_enum.h#L10906-L10913
[dma-events]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_enum.h#L9760-L9768
[master-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L6051-L6063
[master-values]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_enum.h#L2020-L2033
[sqg-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L28893-L28905
[sq-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L29701-L29718
[dma-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L21453-L21479
[sqg-stage-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L29145-L29167
[sq-stage-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L30305-L30341
[compute-enable]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L4774-L4783
[sample-config]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L3878-L3886
[window-versions]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L225-L226
[window-comparison]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L397-L398
[window-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4870-L4912
[window-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L1671-L1684
[begin-control]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2020-L2068
[shader-controls]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2070-L2133
[begin-sample]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2142-L2172
[start-counting]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2174-L2202
[sample-stop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3689-L3750
[windowed-control]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L4062-L4094
[end-control]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2223-L2310
[idle-control]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L4097-L4142
[copy-counter]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L1604-L1624
[copy64]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3948-L3963
[wgp-read]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3797-L3813
[generic-read]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3845-L3895
[stop-workaround]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9SettingsLoader.cpp#L376-L390
[layout-finalize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L1773-L1846
[layout-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L1850-L1888
[session-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/gpuUtil/palGpaSession.h#L479-L503
[sample-end-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/gpuUtil/palGpaSession.h#L673-L688
[profiler-begin]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/layers/gpuProfiler/gpuProfilerCmdBuffer.cpp#L4826-L4884
[profiler-session]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/layers/gpuProfiler/gpuProfilerCmdBuffer.cpp#L4913-L4977
[result-memory]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L4163-L4226
[sample-initialize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1742-L1812
[sample-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1982-L1996
[session-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1537-L1618
[profiler-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/layers/gpuProfiler/gpuProfilerQueueInternal.cpp#L386-L409
[profiler-retire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/layers/gpuProfiler/gpuProfilerQueueInternal.cpp#L1182-L1245
[session-ready]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L2033-L2062
[result-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/gpuUtil/palGpaSession.h#L710-L740
[reset-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/gpuUtil/palGpaSession.h#L820-L829
[result-decode]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSessionPerfSample.cpp#L129-L175
[profiler-results]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/layers/gpuProfiler/gpuProfilerQueueFileLogger.cpp#L703-L765
