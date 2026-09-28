# Command and interop coverage

The grouped corpora provide executable witnesses for the behaviors below.
Each result applies to the selected device, native transport, memory profile,
program images and source revision. A case's presence in this map does not
establish that it ran on a particular deployment. The
[qualification model](qualification.md) describes required cases, result
identity, independent observation and checked retirement.

## Find a behavior

| Behavior | Case sources | What the oracle observes |
| --- | --- | --- |
| PM4 memory operations | [write](pm4/write_test.cc), [copy](pm4/copy_test.cc), [wait](pm4/wait_test.cc), [atomic store](pm4/atomic_store_test.cc) | Changed data, selected widths and extents, surrounding bytes, producer/consumer ordering and CPU handoffs. Atomic-store coverage is distinct from read-modify-write operations. |
| PM4 execution and dependencies | [dispatch](pm4/dispatch_test.cc), [cross-queue handoff](pm4/handoff_test.cc), [indirect dispatch](pm4/indirect_test.cc), [command buffers](pm4/command_buffer_test.cc) | Shader outputs across generations, device-produced dispatch counts, immutable indirect buffers and completed-use command rebuilding. |
| PM4 shader resources | [LDS](pm4/lds_test.cc), [resource changes](pm4/resource_test.cc) | Cross-wave exchange through static/dynamic workgroup storage and transitions between distinct resource configurations. |
| SDMA transfers | [copy](sdma/copy_test.cc), [fill](sdma/fill_test.cc) | Linear copies, byte tails/page crossings, DWORD fills, NOP-separated dependent copies and fence-visible output. |
| AQL publication and dependencies | [publication](aql/publication_test.cc), [barriers](aql/barrier_test.cc), [epochs](aql/epoch_test.cc), [fan-in](aql/fanin_test.cc), [scope](aql/scope_test.cc) | Slot reuse, independent producer publication, AND/OR/value dependencies, complete shader payloads and AGENT-to-SYSTEM scope composition. |
| AQL execution and resources | [dispatch](aql/dispatch_test.cc), [geometry](aql/geometry_test.cc), [scratch](aql/private_test.cc), [LDS](aql/lds_test.cc), [resource changes](aql/resource_test.cc) | Complete/partial multidimensional grids, caller-owned private storage, static/dynamic group storage and resource rebinding. |
| AQL transfer and reuse | [carriers](aql/transfer_test.cc), [byte copy](aql/byte_copy_test.cc), [pattern fill](aql/pattern_fill_test.cc), [executable reuse](aql/executable_test.cc), [worksets](aql/workset_test.cc) | PM4-carried copies, shader subspan operations, completed-use code replacement and independent final-use obligations. |
| CPU/GPU and cross-engine memory edges | [memory pairs](recipes/memory_pair_test.cc), [SDMA/AQL](recipes/copy_dispatch_test.cc), [PM4/SDMA](recipes/pm4_sdma_test.cc) | Queried concrete/profile policies, coherent SYSTEM and staged LOCAL payloads, upload/compute/download and final consumer output. |
| CPU/NPU execution | [CPU/XDNA recipes](../xdna/recipes/README.md) | Allocated/registered backing, queried host publication/acquisition, changed arithmetic outputs, full guards and native retirement. |
| Finite GPU/NPU execution | [GPU/XDNA recipes](../interop/gpu/xdna/recipes/README.md#finite-recipes) | GPU transfers or shaders produce NPU inputs and consume its output; native phases are joined on the host without intermediate CPU payload access. |
| Resident GPU/NPU execution | [resident recipes](../interop/gpu/xdna/recipes/README.md#resident-exchange) | Both dataflow initiators, one/two credits, independently progressing workers, complete per-generation transcripts, backing/payload layouts, startup abort and final drain. The host does not relay intermediate work. |
| Timestamp observations | [PM4 command processor](pm4/timestamp_test.cc), [PM4 shader](pm4/dispatch_test.cc), [AQL](aql/timestamp_test.cc), [SDMA](sdma/timestamp_test.cc) | Sampling order, written result extents, visibility and completed-use reuse. Resident recipes also retain raw GPU clock observations. |

The [wire-format tests](README.md#build-and-execution) check packet encodings
without activating hardware. They do not replace native field or composition
cases. Hardware counters and calibrated performance are separate observation
surfaces; correctness timestamps alone supply neither.

## Deployment paths

The existing case fixtures distinguish these paths. Exact admission also checks
queue capabilities, cache operations, backing roles and compiled program ABI.
The target name alone does not select a valid recipe.

| Deployment | Corpus and native path | Backing and lifetime distinctions |
| --- | --- | --- |
| Linux gfx1151 | PM4 and SDMA USER queues; PM4/SDMA recipes. | Ordinary command storage and control are coherent SYSTEM allocations. Shader fixtures select the exact gfx1151 image. |
| Linux gfx942 | AQL and SDMA USER queues; SDMA/AQL recipes. | Coherent SYSTEM and staged LOCAL payload cases are separate. Scratch, code, arguments and completion have their own retained owners. The gfx1151-specific SDMA NOP case is a separate target predicate. |
| Linux gfx1151 + NPU5 | GPU/XDNA recipes, USER PM4 and native XDNA kernel submissions. | Joint allocation is exercised with PROCESS and INSTANCE lifetimes. Caller-page registration requires PROCESS lifetime on this KFD path. |
| Windows gfx1150 + NPU4 | GPU/XDNA recipes, KERNEL PM4 and native XDNA kernel submissions. | Joint registration is exercised with either lifetime. Joint allocation has no common advertised construction/export route; it is not substituted with registration. |
| Linux NPU5 / Windows NPU4 | CPU/XDNA recipes through native kernel submissions. | Allocation and registration are independently selected from live capabilities. Both image profiles are built from the same Loom source. |

Windows compilation of the ordinary PM4/AQL/SDMA corpora does not establish a
USER queue service there. The GPU/XDNA fixture has its own explicit KERNEL
publication path. Physical peer-GPU execution has
[no compiled cases](peer/README.md).

Additional packet fields, rectangular SDMA transfers, SDMA atomics, general
poll/cache controls, command-buffer variants, physical peers and hardware-counter
collection need their own native witnesses. The corresponding
[hardware reference](../../../docs/reference/amd/gpu/README.md) has a broader
semantic scope than the implemented CTS. A new HAL recipe is qualified by its
complete producer/dependency/consumer/reuse behavior, not by finding its opcode
in an encoder.

## Build and select required cases

Ordinary GPU corpora have the following process-lifetime test targets. Appending
`_instance` selects a second invocation of the same dynamic executable.

| Corpus | Bazel target |
| --- | --- |
| PM4 | `//libamdf/cts/gpu/pm4:pm4_dynamic` |
| SDMA | `//libamdf/cts/gpu/sdma:sdma_dynamic` |
| AQL | `//libamdf/cts/gpu/aql:aql_dynamic` |
| Single-GPU recipes | `//libamdf/cts/gpu/recipes:recipes_dynamic` |
| CPU/NPU recipes | `//libamdf/cts/xdna/recipes:execution_dynamic` |
| GPU/NPU recipes | `//libamdf/cts/interop/gpu/xdna/recipes:execution_dynamic` |

Shader-bearing GPU corpora require Loom's AMDGPU target/emitter; CPU/NPU recipes
require its XDNA target/emitter, and GPU/NPU recipes require both. The SDMA and
host encoding corpora have no shader compiler dependency. The
[source fixture guide](kernels/README.md) describes generated image identities.

A short gfx1151 PM4 execution witness is:

```sh
iree-bazel-test --config=asan \
  //libamdf/cts/gpu/pm4:pm4_dynamic \
  --test_arg="--amdf_gpu_endpoint_id=${GPU_ENDPOINT_ID}" \
  --test_arg=--amdf_gpu_target=gfx1151 \
  --test_arg=--gtest_filter=Pm4DispatchTest.CoherentSystemPayloadChangesAcrossEpochs \
  --test_arg=--amdf_require_test=Pm4DispatchTest.CoherentSystemPayloadChangesAcrossEpochs
```

For a resident GPU/NPU path with registered backing:

```sh
iree-bazel-test --config=asan \
  //libamdf/cts/interop/gpu/xdna/recipes:execution_dynamic \
  --test_arg="--amdf_gpu_endpoint_id=${GPU_ENDPOINT_ID}" \
  --test_arg=--amdf_gpu_target=gfx1151 \
  --test_arg=--gtest_filter=ResidentGpuXdnaTest.RegisteredCausalRoundTrip \
  --test_arg=--amdf_require_test=ResidentGpuXdnaTest.RegisteredCausalRoundTrip
```

Use `gfx1150` for the corresponding Windows resident path. A parameterized
NPU-initiated witness has the full name
`NpuInitiated/ResidentNpuInitiatedTest.ReturnsEveryWord/Rounds17Words16`.
The same suite's `GpuOnlyAbort` and `NpuOnlyAbort` parameters exercise accepted
work draining after the startup decision without a submitted peer.

`--gtest_list_tests` lists the executable's exact names without activating a
device. Each `--amdf_require_test` names one complete case, including its
parameter suffix; it is not a wildcard. A filtered, absent, skipped, unexecuted
or failed required case fails the process. Requirements belong to one corpus
invocation, so different corpora use separate invocations and required lists.
Unfiltered discovery may contain capability skips and is not a substitute for
a required deployment witness.

The examples demonstrate individual behaviors. A qualification record retains
the full selected case list and result XML, native identity, source and binary
identity, generated image hashes, build configuration and checked cleanup.
PROCESS and INSTANCE are separate records. Neither a successful build nor an
older revision's native result certifies the current executable.
