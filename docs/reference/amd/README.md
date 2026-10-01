# AMD hardware reference

This is an **unofficial guide**, assembled through best-effort reverse
engineering and distillation of publicly available repositories and
documentation, together with our own experiments. It is not an official AMD
specification and uses no confidential or non-public vendor material.

Correctness and completeness are not guaranteed. Citations and experimental
results provide evidence for the specific behavior and conditions they cover;
they do not establish correctness on other hardware, firmware, driver or
software configurations.

This reference describes AMD hardware and firmware programming: command and
register representations, execution ordering, memory visibility, resource
lifetime, and architecture-specific behavior. Native driver interfaces and
upstream runtime implementations supply the context needed to use those
mechanisms on a particular platform.

| Area | Contents |
| --- | --- |
| [GPU](gpu/README.md) | Command processors, transfer engines, and their memory and signaling protocols. |
| [XDNA](xdna/README.md) | AI Engine array execution, DMA, interconnects, firmware commands, and observation. |
| [CPU, GPU and NPU interop](interop/README.md) | Every directed memory handoff, external API sharing, resident credits, composed pipelines and final reuse. |
| [Sources](sources.md) | Architecture specifications, immutable implementation revisions, and the role of each source. |
| [Writing guide](STYLE.md) | Chapter structure, terminology, evidence, and citation conventions. |

Each mechanism carries its own architecture, firmware, transport, and memory
conditions. A compiler target, physical engine revision, operating-system
interface, and runtime policy describe different parts of that applicability.
Where sources disagree, the affected fields and source-specific interpretations
remain explicit.

## Programming tasks

The family indexes organize the hardware mechanisms. These routes start from
the operation a native caller needs to perform; packet and register names also
provide exact search terms within the tree.

| Task | Starting points |
| --- | --- |
| Identify the compiler target, physical GC or SDMA IP, and native transport | [Architecture identity and discovery](gpu/architectures.md). |
| Reserve, publish and reuse queue storage | [PM4 ring frontiers](gpu/pm4/publication.md), [AQL header and doorbell publication](gpu/aql/publication.md), [SDMA reservation and ordered commit](gpu/sdma/publication.md). |
| Launch a compiled GPU program | [PM4 `SET_SH_REG` and `DISPATCH_DIRECT`](gpu/pm4/dispatch.md), [AQL kernel dispatch and descriptors](gpu/aql/dispatch.md). |
| Bind shared workgroup storage | [PM4 `LDS_SIZE`](gpu/pm4/lds.md), [AQL group storage](gpu/aql/dispatch.md#static-and-dynamic-group-storage). |
| Determine which agents can access a memory pool | [Pool grain, per-agent access and SVM host access](gpu/recipes/host-device.md#pool-grain-agent-access-and-svm). |
| Make a producer's writes visible to its consumer | [GPU cache controls](gpu/pm4/cache.md), [CPU/GPU handoffs](gpu/recipes/host-device.md), [all six CPU/GPU/NPU directions](interop/README.md). |
| Copy or fill memory and wait for completion | [SDMA packet index](gpu/sdma/README.md), [PM4 `DMA_DATA`](gpu/pm4/dma.md), [SDMA upload → AQL dispatch → SDMA download](gpu/recipes/README.md#sdma-upload-aql-dispatch-and-sdma-download). |
| Replace or reuse commands and executable storage | [PM4 indirect buffers](gpu/pm4/command-buffers.md), [AQL command carriers](gpu/aql/transfers.md), [AQL executable lifetime](gpu/aql/dispatch.md#executable-publication-and-final-use), [SDMA command buffers](gpu/sdma/command-buffers.md). |
| Configure an NPU transfer or split/join flow | [Tile DMA descriptors and task tokens](xdna/dma.md), [stream switches and multicast](xdna/interconnects.md), [pipeline ownership](interop/pipelines.md). |
| Exchange resident GPU/NPU payloads and return credits | [GPU/NPU ready and completion edges](gpu/recipes/gpu-npu.md#resident-programs-and-per-generation-ownership), [slot generations and drain](interop/pipelines.md). |
| Share Vulkan or D3D12 resources with a native consumer | [External memory, handle ownership and dependency objects](interop/external-memory.md). |
| Measure dispatch, transfer or tile execution | [GPU clocks and counter ownership](gpu/observability.md), [PM4 timing bracket](gpu/pm4/timing.md#timestamp-visibility-and-storage-lifetime), [SDMA timestamps](gpu/sdma/timing.md), [XDNA timers, counters and trace](xdna/observability.md). |
