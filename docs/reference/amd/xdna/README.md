# XDNA arrays

XDNA executes programs across an array of AI Engine compute tiles, memory
tiles, DMA engines and stream switches. A firmware controller configures and
orchestrates the array; tile programs perform the computation. Native drivers
create the execution contexts, establish memory access and report submitted
command results.

| Mechanism | Programming contract |
| --- | --- |
| [Native execution](execution.md) | Device identity, placement, instruction submission, program quiescence, memory lifetime and runtime power. |
| [Tile DMA and task ownership](dma.md) | Descriptor fields, address generators, local locks, task queues, completion tokens and buffer reuse. |
| [Interconnects](interconnects.md) | Circuit and packet routes, multicast, backpressure, and split/join buffer ownership. |
| [CPU and array memory handoff](../interop/cpu-npu.md) | Host backing and addresses, CPU cache-maintenance extents, input publication, output completion and slot reuse. |
| [GPU and array memory handoff](../gpu/recipes/gpu-npu.md) | Shared external backing, native addresses, shim DMA completion, resident progress and payload reuse. |
| [Heterogeneous pipelines](../interop/pipelines.md) | CPU/GPU/array composition, returned credits, split/join, nested ownership, progress and drain. |
| [Timing, counters and trace](observability.md) | Tile-clock samples, event counters, stream replies, trace packets and DMA, firmware results and diagnostic access. |

Architecture, product and native transport are separate coordinates. AIE2IPU,
AIE2P and AIE4 have different register and firmware contracts. Product names
such as NPU4, NPU5 and NPU6 identify driver-selected devices; array geometry
and the active firmware interface supply additional constraints. Sharing an
instruction set does not establish equal placement capacity or interchangeable
native command payloads.

The [primary-source map](../sources.md) identifies the specifications, drivers
and upstream consumers. Each chapter cites the implementation or definition
which supplies the particular mechanism's semantics.
