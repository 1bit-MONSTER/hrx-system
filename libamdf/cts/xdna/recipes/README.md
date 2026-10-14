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

`FirmwareResultRecipeTest` attaches the smallest advertised finite result
buffer through the dynamic XDNA API. Four useful numeric commands each emit
three caller-identified timer records, with one host wait after the four
submissions. Two batches append to the same attachment; a third follows an
explicit detach/clear/attach cycle. Repeated collection preserves the snapshot.
The oracle checks all IDs, two adjacent samples with an unchanged timer epoch,
unused result bytes, numeric output, instruction bytes and aperture guard
storage. Native control points do not alter public queue completion identity.
Validation also rejects misaligned, empty and foreign-scope bindings before
native effects. Failed controls preserve backing instead of guessing retirement.

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
