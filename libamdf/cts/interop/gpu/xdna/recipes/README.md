# GPU and XDNA recipe qualification boundary

This package owns compositions whose payload crosses between actual GPU and
XDNA execution. It currently contains no native cases. Existing platform
memory/interop tests remain evidence for allocation, mapping and pair-query
contracts; shared CPU aliases alone do not prove a device-produced data edge.

The first witness needs a GPU producer, a complete system-memory release and
ordering edge, XDNA shim DMA into tile memory, a program-visible consumer and
independent output. Its reverse direction needs XDNA program/DMA completion
before the GPU acquires and reads backing. A backing-level NONE cache answer
does not describe tile-SRAM transport or eliminate DMA completion.

This path inherits both GPU and XDNA build/run requirements and the shared AMD
device resource group. Its executable will use the dynamic provider and cached
device owners just like the engine corpora. A helper addition that changes
shared ownership or observation semantics is reviewed at the common testbench
boundary before either engine relies on it.
