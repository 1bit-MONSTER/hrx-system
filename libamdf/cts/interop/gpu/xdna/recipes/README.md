# GPU and XDNA memory recipes

The grouped `execution` corpus composes GPU memory transfers or
[Loom-built GPU shaders](../../../../gpu/kernels/README.md) with the
[Loom-built XDNA arithmetic program](../../../../xdna/programs/README.md):

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

The recipe admits the GFX11.0/GFX11.5 PM4 encoding supported by the shared
packet writer and the NPU4 Strix/Krackan or NPU5 Halo finite program. A capable
USER queue publishes complete packets without splitting them across the ring
boundary; a coherent marker establishes completion before payload observation,
and the read frontier separately retires command storage. An advertised KERNEL
queue instead uses private command storage and checked native completion.
Transport selection precedes queue creation and never changes after a native
failure. Shader cases additionally require the COMPUTE role and the exact
gfx1150 or gfx1151 target of their compiled image. Selection uses the GPU's
reported IP, independently of the host OS and queue transport.

Queue destruction precedes release of reachable backing. XDNA instruction
storage and context outlive its queue; registered CPU storage outlives its
attachments. A failed native release stops destruction of dependent owners.

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
