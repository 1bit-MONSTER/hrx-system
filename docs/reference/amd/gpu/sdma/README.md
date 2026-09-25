# SDMA

SDMA transfers data and executes control operations independently of shader
dispatch. Its packet representation, memory routing, and completion protocol
depend on the engine generation and native transport.

| Topic | Mechanisms |
| --- | --- |
| [Linear copy](copy.md) | Byte ranges, count representation, runtime caps, alignment and chunking. |
| [Constant fill](fill.md) | Pattern width, count units, generation differences and completion. |
| [Ordering](ordering.md) | Pending-transfer drains, overlap, NPD and resource ownership. |
| [Completion stores](fence.md) | FENCE32, per-generation policy fields, notification and 64-bit forms. |
| [Memory dependencies](poll.md) | Comparisons, retry controls, signal lifetime and scoped/64-bit layouts. |
| [Atomic operations and signaling](atomics.md) | Copy-completion decrements, command-retirement increments, ring semaphores, and fused copy signaling. |
| [Cache maintenance](cache.md) | USER_GCR, scheduled kernel GCR, HDP and command publication. |
| [Command buffers](command-buffers.md) | Scheduled IBs, context operands, direct rings and storage retirement. |
| [Timestamps](timing.md) | Global clock samples, transfer ordering and interval interpretation. |

Payload visibility, a control-word update, notification, and storage
retirement are separate edges. The programming sequences identify which
operation establishes each edge and which actor still owns the referenced
memory.
