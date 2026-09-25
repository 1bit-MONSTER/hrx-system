# GPU command conformance

The command CTS exercises the public libamdf ABI through one runtime-loaded
shared-library executable per native corpus. An operation or field combination
gets a named GoogleTest case, not a separate executable. Sources remain
separate translation units so the build can compile them concurrently.

The [hardware reference](../../../docs/reference/amd/gpu/README.md) describes
native command semantics. The [library queue guide](../../docs/gpu.md) describes
the public caller contract. Neither source membership nor packet definitions
alone establish a deployment's executed behavior.

## Physical boundaries

```text
gpu/
  gpu_device_fixture.h       # Passive selection and the existing device cache.
  util/                      # Case-owned memory and mapped user queues.
  pm4/                       # One native PM4 corpus.
    encoding/                # Host-only wire-format checks and test encoder.
  sdma/                      # One native SDMA corpus.
    encoding/
  aql/                       # One native AQL corpus and publication helper.
    encoding/
  recipes/                   # One single-GPU composition corpus; N queues.
  kernels/                   # Compiler fixture/provenance boundary.
  peer/                      # Physical multi-GPU deployment boundary.
  lifecycle/                 # Completed resources and opt-in device recreation.
```

`interop/gpu/xdna/recipes/` owns the GPU/NPU composition boundary. Existing
platform memory, kernel-queue and external-API suites retain their separate
dependencies. The `kernels` package owns compiled kernel fixtures and their
provenance; `peer` and XDNA recipe packages currently export their
design/readme only, with no placeholder tests or executables.

`CtsDeviceCache` creates one instance and one device per endpoint and engine
kind for an executable, caching activation failures as well. GPU queue families
on the same endpoint share that cached GPU device. `GpuCommandTest` borrows it
and owns stable case-local memory and queue collections. Each case can create
N queues without creating N devices. Explicit cleanup precedes provider unload,
including partial initialization and failed native removal paths.

The shared utilities expose memory contracts and mapped producer state. They
do not insert cache commands, poll payload memory or wait for a producer as a
side effect of constructing a consumer. Engine fixtures own packet publication
and exact completion semantics. Resource setup is test infrastructure; none of
these collections or helpers becomes production queue scheduling machinery.

Dataflow cases observe their payloads after the named completion and before
polling ring consumption. Independent CPU expectations cover the initialized
payloads and guards; input preservation is checked separately. Oracle failures
still reach retirement before teardown or any reuse. Ring-consumption checks
remain separate from execution completion and payload visibility.

## Build and execution

`//libamdf/cts/gpu/{pm4,sdma,aql,recipes}` each uses `amdf_cts_test_suite` with
the default dynamic provider. Process/instance native lifetimes are two test
invocations of the same binary. Only `//libamdf/cts/core:query` explicitly
retains the three binding modes.

Native corpora require x86-64 and Linux or Windows at compile time, inherit
the `libamdf.resource.amd_gpu` execution requirement and share the AMD GPU
resource group. Encoder target predicates and family capabilities select the
actual native queue service. Windows compilation of a user-ring corpus does
not imply Windows user-ring execution.

Each `encoding/` package has one plain host-test binary. Package policy removes
the GPU execution requirement for these exact packages, so byte-layout checks
run without a GPU and do not reserve a GPU slot. GPU-family build enablement
still applies. Native cases acquire neither Vulkan/D3D12 dependencies nor a
shader compiler merely by using a GPU.

Bazel declarations are authoritative; generated CMake targets preserve the
same corpus, dynamic loading, requirements and resource group. For example,
`libamdf_cts_gpu_recipes_recipes_dynamic_bin` is the CMake build target, and
`libamdf/cts/gpu/recipes/recipes_dynamic` is its process-lifetime CTest name.
The [qualification invocation](qualification.md#required-witnesses-and-deployment-identity)
adds an exact endpoint ID, required case names and an expected compiler target.

The manual lifecycle corpus has ordinary PM4/SDMA cases that copy between
exact-access attachments on the cached device, observe both complete pages
before retirement, and release their queue before its backing. SDMA uses the
coherent SYSTEM COPY/FENCE recipe; PM4 retains its explicit cache transitions.
These cases exercise the same resource helper as the `DISABLED_` peer-device
recreation scenarios, without creating extra devices. Recreation requires
`--gtest_also_run_disabled_tests` and is a separate qualification. The manual
corpus is not part of the ordinary command aggregate. Other API/interop suites
can have intentional owner-lifetime tests of their own.

## Category and source map

The table links the authoritative case lists in each corpus's Bazel declaration.
Additional translation units enter its explicit `srcs`; adding a file does not
create another executable. Source membership is separate from the recorded
native coverage of each deployment. The operation references describe the
remaining field, composition and architecture boundaries within each group.

| Corpus | Authoritative case sources | Other qualification boundaries |
| --- | --- | --- |
| PM4 | [pm4/BUILD.bazel](pm4/BUILD.bazel) | Publication, additional atomic operations/backing, cache policies, indirect-buffer lifetime and counter ownership. |
| SDMA | [sdma/BUILD.bazel](sdma/BUILD.bazel) | Additional fence/poll fields, atomic reach, cache policies, indirect buffers and counter ownership. |
| AQL | [aql/BUILD.bazel](aql/BUILD.bazel) | Signal reach, additional executable lifecycles, profiling, counters and metadata. |
| Recipes | [recipes/BUILD.bazel](recipes/BUILD.bazel) | Additional backing classes, producer/consumer compositions and executable visibility. |
| Manual lifecycle | [lifecycle/BUILD.bazel](lifecycle/BUILD.bazel) | Ordinary same-device copies are enabled; peer-device recreation remains disabled. |
| Physical peers | No compiled cases | Multi-device admission, address reach, synchronization and runner requirements. |
| GPU/NPU recipes | No compiled cases | Device-produced payloads and both sides' visibility and DMA/channel contracts. |

Cases use real commands and changing exact data. They do not exhaust their
opcodes' fields. New cases add independent oracles, legal field partitions and
compositions to the corresponding engine group. Shared testbench changes affect every corpus and need
their own caller/ownership review before dependent cases consume them.

Timing cases qualify the observation itself: sampling point, ordering, result
width and visibility. Clock calibration, hardware-counter ownership and
optimized latency/throughput experiments have their own
[measurement contracts](../../../docs/reference/amd/gpu/observability.md). Raw timestamp properties
from correctness tests carry no nanosecond or performance interpretation.
