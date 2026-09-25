# CPU and XDNA memory recipes

The grouped `execution` corpus runs a finite arithmetic program through the
runtime-loaded public API. Its [ELF reader](../util/executable.h) and
[checked-in program](../programs/README.md) are CTS-only. Building and running
the corpus needs neither a compiler for the array nor a runtime image loader.

`CpuXdnaRecipeTest.AllocatedRoundTrip` uses system allocations accessible to
the NPU. `RegisteredRoundTrip` retains separately allocated CPU storage through
registration, execution, unmapping and native attachment release. Each case
borrows the cached device and owns its context, queue and memory resources.
Process and instance native lifetimes are separate invocations of the same
dynamic executable.

Each case performs eight generations of sixteen unsigned 32-bit products.
Inputs change between generations, output words start with the complement of
the expected result, and every byte outside the logical payload is guarded.
The oracle checks both inputs, the output, allocation-granularity padding and
the complete command owner. Repeated submissions retain the same bound command
bytes and independently queried DMA addresses.

The case queries both directional memory edges before execution. CPU
publication uses the mapping's exact flush recipe; CPU acquisition uses its
exact invalidate recipe. The device-side actions are NONE. Command publication
is separate and explicit. The worker releases its data locks and terminates;
the controller joins the core and every used DMA channel. Checked native
completion precedes output-first snapshots and all numerical comparisons.
Neither queue progress nor a cache action substitutes for those joins.

The queue is destroyed before its instruction storage and context. Payload
attachments are released before registered CPU backing. A failed native
release stops destruction of dependent owners.

```sh
iree-bazel-test --config=asan //libamdf/cts/xdna/recipes:execution
iree-cmake-test -R '^libamdf/cts/xdna/recipes/execution_dynamic'
```

The package carries the XDNA build/run requirement and the shared AMD hardware
resource group. Exact qualification invocations require both test names with
`--amdf_require_test=Suite.Case`; a missing target fixture or skipped required
case cannot count as successful execution. The fixtures cover the NPU4
Strix/Krackan and NPU5 Halo execution profiles.
