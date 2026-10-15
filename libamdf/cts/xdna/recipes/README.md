# CPU and XDNA memory recipes

The grouped `execution` corpus runs a finite arithmetic program through the
runtime-loaded public API. Its [ELF reader](../util/executable.h) and
[Loom program](../programs/README.md) are CTS-only. The ordinary build compiles
the checked-in `.loom` source and embeds the native images. The test executable
uses no runtime image-loader dependency.

`CpuXdnaRecipeTest.AllocatedRoundTrip` uses system allocations accessible to
the NPU. `RegisteredRoundTrip` retains separately allocated CPU storage through
registration, execution, unmapping and native attachment release. Each case
borrows the cached device and owns its context, queue and memory resources.
Process and instance native lifetimes are separate invocations of the same
dynamic executable.

The round-trip cases perform eight generations of sixteen unsigned 32-bit
products.
Inputs change between generations, output words start with the complement of
the expected result, and every byte outside the logical payload is guarded.
The oracle checks both inputs, the output, allocation-granularity padding and
the complete command owner. Repeated submissions retain the same bound command
bytes and independently queried DMA addresses.

`AllocatedFirstDeviceWriter` and `RegisteredFirstDeviceWriter` leave output
payload untouched on the CPU until the first completed NPU overwrite. Only
inputs and commands are published before submission; output is acquired once
after native completion and checked against sixteen nonzero products.
`RegisteredDirtyFirstDeviceWriter` writes old contents before registration,
then follows that same first-use path without a caller output flush and checks
that neighboring caller contents survived registration and execution. These
cases exercise initial cache readiness independently of the guarded round-trip
tests, whose output initialization already requires a CPU release.

`AllocatedRoundTripWithFirmwareTrace` incrementally reads the automatic
firmware recorder while 16,384 unchanged numeric commands execute. It uses
fixed caller storage, checks native record continuity and complete command
intervals, and restores its initially disabled recorder. Unsupported native
trace services skip this case without changing ordinary workload coverage.

`FirmwareDurationsAndHostClockBounds` executes 32 guarded numeric commands
with a deliberate 100 ms ordinary idle gap midway through the capture. It
checks positive nanosecond firmware intervals, source ordering, containment
within each host submission/completion width, and records the corresponding
source-to-host offset bounds. Its XML properties retain observed durations,
the final offset range, and whether a constant-offset model fits the capture.
Independent host and firmware clocks can drift even when both report
nanoseconds; an empty intersection rejects that model without failing timestamp
publication. These observations report sampling uncertainty, not latency
thresholds or benchmark results. The case uses the same recorder and ordinary
completion protocol as the streaming case, with bounded caller-owned calibration
storage.

`FirmwareTraceRecipeTest.AttributesConcurrentContextLifetimes` keeps two
independent numeric programs and queues alive on the shared device. Both
commands are submitted before either completion wait, with alternating host
submission order and distinct guarded payloads. After eight rounds it destroys
one context, creates another, and runs eight more rounds beside the survivor.
Native context queries supply each owner's physical firmware slot; intact
create/delete records and per-context command ordering supply the finite
attribution interval. Slot reuse does not merge the retired and new owners.

The case checks all 32 numeric results, unchanged command storage, complete
lifetime and command pairs, native read continuity, and each firmware duration
against its own host bracket. XML retains host clock units, per-owner intervals,
queried identities, slot reuse, interval overlap, and constant-offset bounds.
Concurrent queue ownership does not require simultaneous device execution.
The proof requires a complete finite capture: loss, ambiguous lifetime records,
or changed native identity cannot be interpreted as a successful historical
join. It skips when either optional native recorder or identity service is
unavailable, and restores recorder configuration during teardown.

`FirmwareResultRecipeTest` attaches the smallest advertised finite result
buffer through the dynamic XDNA API. Four useful numeric commands each emit
three caller-identified timer records, with one host wait after the four
submissions. Two batches append to the same attachment; a third follows an
explicit detach/clear/attach cycle. Repeated collection preserves the snapshot.
The oracle checks all IDs, an interval from core enable through output DMA
completion after configuration, unused result bytes, numeric output, instruction
bytes and aperture guard storage. Native control points do not alter public
queue completion identity.
Validation also rejects misaligned, empty and foreign-scope bindings before
native effects. Failed controls preserve backing instead of guessing retirement.

`NativeArraysAppendCollectAndRearm` applies the same numerical, identity,
immutability, and lifetime checks to 65 commands per batch. Commands are grouped
up to the queue's queried native array limit, with one completion wait after
the full batch. The finite destination covers two appended batches before
rearming, including capacity beyond one page. This exercises marker publication
across native command-array boundaries independently of host collection.

`MarkerTicksAndRateSnapshots` uses the native timer clock query after context
admission. Three four-command batches execute 1, 4096 and 16384 recurrence
iterations per output word. Each batch has one completion wait, then finite
collection and an independent numerical oracle. Same-command marker ticks
must advance, with record IDs, numeric results, and storage guards preserved.
`MarkerTicksAndFirmwareIntervals` also records the independent automatic
firmware intervals when that service is admitted. Those serial nanosecond
intervals must fit their enclosing host batch.

Both cases retain raw tick endpoints, queried rates, host brackets, and an
explicit endpoint-rate estimate in XML. `endpoint_rate_model_fits` evaluates
whether equal endpoint rates and that estimate are consistent with all
available brackets. A false result rejects the fixed-rate interpretation;
a true result does not establish constant frequency. A changed endpoint rate
still runs all batches and checks their native records and numeric output.
Ordinary execution supplies no controlled-rate interval, and these cases
change no machine power policy. Nanosecond conversion of markers requires a
separate rate/continuity contract; automatic firmware nanoseconds remain
independent of the array rate. These cases do not qualify performance
thresholds or cross-command counter continuity.

`XdnaReservationTest` queries native reservation support and mapping-source
eligibility before allocating backing. It keeps one command and queue alive
while replacing inputs and outputs at fixed NPU addresses, including repeated
physical aliases, untouched neighboring mappings, partial removal and refill,
and checked whole-reservation detach. Every epoch verifies both complete retained
backings, including bytes at the old addresses. A separate case leaves the new
output backing untouched until the first completed device write. Allocation and
registration roles each run with native geometry and a larger caller-selected
mapping unit. CPU/GPU/NPU composition is covered by the
[fabric recipes](../../interop/gpu/xdna/recipes/README.md).

The ordinary round-trip cases query both directional memory edges before execution. CPU
publication uses the mapping's exact flush recipe; CPU acquisition uses its
exact invalidate recipe. The device-side actions are NONE. Command publication
is separate and explicit. The finite output DMA join ends this program's
external payload accesses; its worker and compute DMA retain only tile-local
state. Every generation submits the complete establishing invocation, which
resets that local state before reconfiguration. Checked native completion
precedes output-first snapshots and all numerical comparisons.

The queue is destroyed before its instruction storage and context. Payload
attachments are released before registered CPU backing. A failed native
release stops destruction of dependent owners.

Enable `LOOM_BUILD` and `LOOM_TARGET_XDNA` alongside `AMDF_BUILD` to build
the compiler-generated fixtures.

```sh
iree-bazel-test --config=asan //libamdf/cts/xdna/recipes:execution
iree-cmake-test -R '^libamdf/cts/xdna/recipes/execution_dynamic'
```

The package carries the XDNA build/run requirement and the shared AMD hardware
resource group. Exact qualification invocations require both test names with
`--amdf_require_test=Suite.Case`; a missing target fixture or skipped required
case cannot count as successful execution. The fixtures cover the NPU4
Strix/Krackan and NPU5 Halo execution profiles.
