# GPU and XDNA memory recipes

The grouped `execution` corpus composes GPU memory transfers or
[Loom-built GPU shaders](../../../../gpu/kernels/README.md) with the
[Loom-built XDNA programs](../../../../xdna/programs/README.md). Finite recipes
join each device phase on the host. The resident recipe submits each device
once and exchanges requests and responses without host intervention.

## Finite recipes

The finite arithmetic flow is:

```text
CPU staging -> GPU ingress -> XDNA DMA and arithmetic -> GPU egress
                                                            |
CPU observation <-------------------------------------------+
```

`GpuXdnaRecipeTest.AllocatedRoundTrip` uses system allocations with joint GPU
and XDNA access. `RegisteredRoundTrip` retains separately allocated CPU storage
through registration and both devices' use. Each case borrows one cached device
per endpoint and owns its queues, XDNA context and memory resources.
`GpuXdnaShaderRecipeTest` runs the same two memory roles with shader ingress and
egress. All cases run through the runtime-loaded shared library. Process and
instance native lifetimes are separate invocations of the same executable.

The test queries six directional pairs on joint backing and two on GPU staging,
both prospectively and for the concrete allocations. GPU commands perform the
queried global system-scope release and acquire operations. Host publication
and acquisition use their directional mapping recipes. XDNA backing actions
are NONE; the command joins the finite external payload flow. Its resident
worker and compute DMA access only tile-local state, which the next establishing
invocation resets before reconfiguration. Each phase completes before the next
phase begins. The CPU neither touches nor maintains joint payload between GPU
ingress and GPU readback.

Each case performs eight generations of sixteen unsigned 32-bit products.
In transfer cases, GPU ingress copies changing inputs and poisoned output words
from staging. The NPU computes the products, and GPU egress copies the inputs, output and
their adjacent guards to separate readback storage. The test captures the
complete GPU readback owner before inspecting joint allocations or command
storage. It then checks all payloads, guards, allocation padding and immutable
NPU command bytes. Later host maintenance cannot repair the captured GPU
readback.

Shader ingress launches two instances of `transform.loom`, each computing
`3*x+addend` modulo 2^32 into one NPU input. Shader egress applies the same
transform to the NPU products in a separate result slice, then performs the
guarded raw readback. Odd, generation-dependent addends make every shader's
effect distinguishable from an identity operation. The independent host oracle
checks transformed inputs, NPU products and final shader outputs. A full
64-workitem group processes sixteen values, so adjacent guards also check the
inactive workitems. Both shader phases join shader stores and perform explicit
system-scope cache actions before their completion edge.

Shader code and three argument slots have GPU-only attachments. Their exact
HOST-to-GPU pair recipes publish them separately from joint payload. Code is
immutable; argument slots change only after final egress retires. Complete code,
prefetch padding, argument slots and allocation padding are checked after GPU
readback is captured. The source build checks compiled argument offsets, types,
resource inputs, wave width and workgroup geometry against the PM4 caller.

The recipe selects its RDNA PM4 encoding from the shared
[command profile](../../../../gpu/pm4/encoding/profile.h) and the NPU4
Strix/Krackan or NPU5 Halo finite program. A capable
USER queue publishes complete packets without splitting them across the ring
boundary; a coherent marker establishes completion before payload observation,
and the read frontier separately retires command storage. An advertised KERNEL
queue instead uses private command storage and checked native completion.
Transport selection precedes queue creation and never changes after a native
failure. Shader cases additionally require the COMPUTE role and select the
exact physical target from the [compiled kernel set](../../../../gpu/kernels/README.md).
The same Loom source supplies each target variant. Selection uses the GPU's
reported IP, independently of the host OS and queue transport.

Queue destruction precedes release of reachable backing. XDNA instruction
storage and context outlive its queue; registered CPU storage outlives its
attachments. A failed native release stops destruction of dependent owners.

## Resident exchange

`ResidentGpuXdnaTest.RegisteredCausalRoundTrip` runs one GPU invocation and one
finite NPU service firing. Both programs are authored in `resident_exchange.loom`
in their respective fixture packages. After both native submissions succeed,
the host publishes RUN in a separate startup allocation. It then joins terminal
completion without reading payloads, updating generations, maintaining shared
payload caches, or submitting per-round work.

Each generation sends a configured number of 32-bit values using one or two
paired request/response slots. Each slot forms a separate causal chain: the GPU
forms its next request from the first value of that slot's preceding response.
The NPU transforms every value and publishes the new response generation. The
GPU records every response in a separate transcript. The CPU checks that complete
causal sequence, final device records, guards, allocation padding, and
immutable command/code storage after both devices retire. A later host
invalidation cannot repair a value already consumed by the GPU and recorded
in that transcript.

Request and response occupy separate allocations. Each slot has one writer per
direction and a leading generation word. The GPU uses system-scope
release/acquire operations. The NPU starts a fresh DMA read for each control
observation and chains its response payload and ready writes on one output
channel with a lock dependency. The final GPU acknowledgement ends custom NPU issuance
before the ordinary terminal output and native DMA idle checks close
external-memory use. Only then may the host release backing.

The GPU initially publishes as many requests as the configured credit count
allows. It returns each response slot's credit by publishing the next request
for that slot after all previous response reads complete. No additional credit
record or host action is needed. The NPU admits every request in each one- or
two-request batch before producing its first response, then services the batch
in order. This requires two outstanding requests in a full two-credit batch;
it does not require concurrent tile arithmetic. An odd final batch uses only
the first slot. The two-credit anchor reuses both slots and finishes with an
odd tail.

The compiler owns its ordinary configuration input, terminal output and tile
program. A constrained transaction composer adds disjoint direct-stream routes
and six or ten shim descriptors around the unchanged bound compiler invocation,
depending on the credit count. This keeps the compiler's executable format and
resource ownership intact.

Before RUN, an accepted participant can observe ABORT and terminate without
waiting for a peer whose submission failed. A failed publication or terminal
join does not establish cancellation; the caller retains reachable owners.
The raw 32-bit device clock samples at the start and end of each exchange are
retained as result properties. Their modular differences and correlation with
external clocks require a separately established clock contract. The samples
are observations from the correctness workload, not calibrated latency results.

`StartupAndBacking/ResidentExchangeTest` varies allocated/registered backing,
GPU-first/NPU-first submission, and zero/one/257 complete generations. These
cases use the same compiled products and exercise unsigned payload wrapping.
`StartupAndBacking/ResidentPrestartAbortTest` submits only the GPU or only the
NPU, publishes ABORT, and checks that it drains without peer progress or
payload changes. These are normal protocol paths with valid native submissions.

`TwoCreditsAndBacking` applies the same startup and abort coverage with two
credits, including zero/one/two/three/17/257/258-generation runs in both launch
orders.

`PayloadAndBacking/ResidentPayloadTest` exchanges 1, 4, 15, 16, 17, 64, or 1024
words per generation with both backing roles. The payload begins either four
or 64 bytes after the generation word, exercising first-line sharing and
separation around cache-line and page boundaries. All sizes use the same GPU
and NPU products; runtime arguments, immutable configuration and descriptor
lengths agree on the exact extent. The full transcript remains the oracle for
every response word. The fixed terminal record additionally carries the word
count and final response's first word, last word and unsigned sum.

`TwoCreditsPayloadAndBacking` applies both placements and backing roles to
two-credit exchanges of 1, 16, or 1024 words. Each slot's complete extent is
rounded up to 64 bytes; the oracle checks inter-slot padding, unused slots,
outer guards and allocation padding as well as the complete transcript.

### Independent workers

`ResidentGpuXdnaTest.RegisteredIndependentChannels` gives two NPU workers
separate request/response channels in one two-column context. Both run the
same resident service program. One GPU invocation completes A1, then three
exchanges with B, then A2. A's next request remains unpublished throughout
B's exchanges. The mirrored case reverses the worker roles.

Every request derives from the immediately preceding actual response, including
the A-to-B and B-to-A transitions. The transcript records channel, local
generation, cause, timestamps and every response word. The independent CPU
oracle checks this chronological chain and each worker's final payload and
terminal summary. This demonstrates useful peer progress while one channel
awaits work; it does not assert simultaneous arithmetic or scheduling fairness.

The separate GPU [program](../../../../gpu/kernels/resident_channels.loom)
uses one credit per worker. Each column owns its descriptors, direct-stream
routes and final acknowledgement. Both ordinary terminal transfers and all
four custom DMA idle observations precede NPU command completion. The host
publishes RUN only after both native submissions, then joins their final
completion without intermediate actions. Sole-GPU and sole-NPU prestart ABORT
cases check that both channels terminate without peer traffic.

`IndependentChannelsAndBacking` varies both worker roles, both submission
orders, allocated/registered backing and one/17/257 peer exchanges. Its abort
cases submit either participant alone for both worker roles and backing types.
`IndependentChannelsPayloadAndBacking` adds one-, 16- and 1024-word payloads
with shared or separate first lines. The startup matrix already covers the
16-word separate-line shape; the payload matrix covers the remaining shapes
with NPU-first submission. The peer's initial cause matches the corresponding
single-worker case, so their complete peer payload sequences are identical.

### NPU-initiated dataflow

`NpuInitiated/ResidentNpuInitiatedTest` starts with an NPU-produced payload.
The GPU consumes it and returns transformed words, which the NPU consumes to
produce the next payload. Both programs are separately authored in
`resident_npu_initiated.loom`; native submission order alone does not reverse
which device produces the first live data.

For positive GPU return count `N`, payload word `i` follows this recurrence
modulo 2^32:

```text
Q1[i]     = seed + 257 + 17*i
Rg[i]     = 3*Qg[i] + g                 for g = 1..N
Q(g+1)[i] = Rg[i] + 257*(g+1) + 17*i  for g = 1..N
```

The immutable seed goes only to the NPU. The GPU records every word of
`Q1` through closing `Q(N+1)`, making even the final NPU consumption visible
to the independent wordwise oracle. It produces no return for the closing
payload. The physical writers stay fixed: the request allocation carries GPU
returns `R`, and the response allocation carries NPU payloads `Q`.

One paired slot provides one credit. Publishing `Rg` after the GPU's reads
returns the Q slot to the NPU. Publishing `Q(g+1)` after all NPU input reads
returns the R slot to the GPU. The NPU streams each input word directly into
its disjoint output path; it needs no complete intermediate vector in tile
memory. The existing final GPU acknowledgement, ordinary NPU terminal transfer,
custom DMA idle observations and both native joins close ownership.

The initial cases exercise one and seventeen GPU returns with one- and
sixteen-word payloads, registered backing, and separate payload/ready cache
lines. `NpuInitiatedAndBacking` expands this to allocated and registered
backing, both submission orders, and zero/one/17/257 GPU returns at sixteen
words. Payload coverage uses NPU-first submission and 1, 4, 15, 16, 17, 64, or
1024 words beginning four or 64 bytes after ready. The matrices share the same
compiled programs and omit shapes already covered by the initial cases.

Zero returns produce no payload or transcript. Sole-GPU and sole-NPU prestart
ABORT cases terminate without peer traffic for both backing roles. All cases
retain complete guards, padding, immutable storage and checked cleanup.

Each transcript row contains Q generation, R generation (zero when closing),
two raw GPU clock samples and all Q words. The cold sample begins at GPU
observation of RUN. The end sample drains Q reads, transcript stores and any R
payload stores; the GPU carries it as the next start before releasing R-ready.
The end sample's own transcript store follows the sample and is drained by the
next release. For positive N the chronological partition is one cold row,
`N-1` steady cycles and one closing row without further GPU-return production.
The `resident_cycle_*` properties retain this distinction and the test checks
carried-sample equality. Final acknowledgement and native joins lie outside
these intervals. Raw correctness samples are not calibrated performance data.

## Build and execution

The ordinary build compiles the `.loom` fixtures and embeds the GPU image and
both NPU profiles. Enable `LOOM_BUILD`, `LOOM_TARGET_AMDGPU` and
`LOOM_TARGET_XDNA` alongside `AMDF_BUILD`.

```sh
iree-bazel-test --config=asan //libamdf/cts/interop/gpu/xdna/recipes:execution
iree-cmake-test -R '^libamdf/cts/interop/gpu/xdna/recipes/execution_dynamic'
```

The package inherits both GPU and XDNA build/run requirements and the shared
AMD hardware resource group. Exact qualification uses `--amdf_gpu_target` and
`--amdf_require_test=Suite.Case` so a skipped required case fails the invocation.
A memory role is skipped when the device or joint profile does not advertise
it. KFD instance lifetimes expose joint allocation but not caller-page
registration; process lifetimes exercise both paths. Windows exposes joint
registration: its GPU and XDNA system allocators have no common construction
or export/import route for joint allocation. No case substitutes a different
memory role after admission or native failure. Result properties record the
selected targets, transport, queue families, memory geometry and generation
count.
