# SDMA

SDMA transfers data and executes control operations independently of shader
dispatch. Its packet representation, memory routing, and completion protocol
depend on the engine generation and native transport.

| Topic | Mechanisms |
| --- | --- |
| [Engine selection](engine-selection.md) | Ordinary/xGMI queue pools, directed engine masks, copy executors, runtime blit/native engine identities, read-only queue observations, and topology routing. |
| [Queue publication](publication.md) | Byte frontiers, reservation and ordered commit, wrap/padding, native visibility and storage ownership. |
| [Linear copy](copy.md) | `COPY_LINEAR`: byte ranges, count representation, runtime caps, alignment and chunking. |
| [Rectangular copy](rectangular-copy.md) | `COPY_LINEAR_SUBWIN` / `COPY_LINEAR_RECT`: element units, row/slice pitches, subwindow layouts, tiling and geometry-specific cache controls. |
| [Constant fill](fill.md) | `CONST_FILL`: pattern width, count units, generation differences and completion. |
| [Inline data writes](write.md) | `WRITE_LINEAR` / `WRITE_UNTILED`: command-carried DWORD data, policy layouts, dependent transfers and storage lifetime. |
| [Ordering](ordering.md) | Pending-transfer drains, overlap, NPD and resource ownership. |
| [Completion stores](fence.md) | `FENCE` / `FENCE_64B`: per-generation policy fields, notification and store widths. |
| [Memory dependencies](poll.md) | `POLL_REGMEM` / `POLL_MEM_64B`: comparisons, retry controls, signal lifetime and scoped layouts. |
| [Atomic operations and signaling](atomics.md) | `ATOMIC` (`ADD64`), `MEM_INCR`, `SEMAPHORE` and fused copy signaling: distinct completion and retirement protocols. |
| [Cache maintenance](cache.md) | USER_GCR, scheduled kernel GCR, HDP and command publication. |
| [Command buffers](command-buffers.md) | Scheduled IBs, context operands, direct rings and storage retirement. |
| [Timestamps](timing.md) | Global clock samples, transfer ordering and interval interpretation. |

Payload visibility, a control-word update, notification, and storage
retirement are separate edges. The programming sequences identify which
operation establishes each edge and which actor still owns the referenced
memory.
