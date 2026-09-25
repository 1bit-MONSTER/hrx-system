# GPU programming

AMD GPUs expose command-processing and transfer engines with distinct packet
representations, execution boundaries, and cache responsibilities. A command
stream combines these mechanisms with the native queue's publication and
resource-lifetime protocol.

| Area | Topics |
| --- | --- |
| [Architecture and transport](architectures.md) | Physical IP, compiler targets, native queues, and submission ownership. |
| [PM4](pm4/README.md) | Compute dispatch, memory commands, cache control, and command-buffer execution. |
| [AQL](aql/README.md) | Packet publication, dispatch, dependencies, vendor command carriers, and profiling. |
| [SDMA](sdma/README.md) | Transfer-engine operations and signaling protocols. |
| [Programming recipes](recipes/README.md) | Host/device, cross-engine, and local-memory producer/consumer flows. |
| [CPU, GPU and NPU interop](../interop/README.md) | External sharing, directed device handoffs, resident pipelines and storage reuse. |
| [Timing and counters](observability.md) | Clock domains, timestamp conversion, counter ownership, and profiling interference. |

Compiler target, physical graphics/compute IP, SDMA IP, firmware, and native
transport identify different parts of a programming contract. The operation
chapters retain those conditions beside the packet fields and sequences they
govern.
