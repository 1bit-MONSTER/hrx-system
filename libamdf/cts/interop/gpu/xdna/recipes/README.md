# GPU and XDNA memory recipes

The grouped `execution` corpus composes GPU memory transfers with the
[Loom-built XDNA arithmetic program](../../../../xdna/programs/README.md):

```text
CPU staging -> GPU TC/L2 copies -> XDNA DMA and arithmetic
                                      |
CPU observation <- GPU TC/L2 readback <-+
```

`GpuXdnaRecipeTest.AllocatedRoundTrip` uses system allocations with joint GPU
and XDNA access. `RegisteredRoundTrip` retains separately allocated CPU storage
through registration and both devices' use. Each case borrows one cached device
per endpoint and owns its queues, XDNA context and memory resources. Both cases
run through the runtime-loaded shared library. Process and instance native
lifetimes are separate invocations of the same executable.

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
GPU ingress copies changing inputs and poisoned output words from staging.
The NPU computes the products, and GPU egress copies the inputs, output and
their adjacent guards to separate readback storage. The test captures the
complete GPU readback owner before inspecting joint allocations or command
storage. It then checks all payloads, guards, allocation padding and immutable
NPU command bytes. Later host maintenance cannot repair the captured GPU
readback.

The recipe admits the GFX11.0/GFX11.5 PM4 encoding supported by the shared
packet writer and the NPU4 Strix/Krackan or NPU5 Halo finite program. A capable
USER queue publishes complete packets without splitting them across the ring
boundary; a coherent marker establishes completion before payload observation,
and the read frontier separately retires command storage. An advertised KERNEL
queue instead uses private command storage and checked native completion.
Transport selection precedes queue creation and never changes after a native
failure. These are TC/L2 transfer tests, not shader execution tests.

Queue destruction precedes release of reachable backing. XDNA instruction
storage and context outlive its queue; registered CPU storage outlives its
attachments. A failed native release stops destruction of dependent owners.

The ordinary build compiles the `.loom` fixture and embeds both NPU profiles.
Enable `LOOM_BUILD` and `LOOM_TARGET_XDNA` alongside `AMDF_BUILD`.

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
