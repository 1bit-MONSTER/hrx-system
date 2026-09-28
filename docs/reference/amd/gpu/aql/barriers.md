# AQL barriers and signals

AQL barriers delay later packet processing until their dependencies are
satisfied. Header fence fields separately establish memory visibility. A
dependency becoming ready, a fence completing, and a packet's completion
signal being decremented occupy different phases of the HSA packet-processing
model. [HSA System Architecture 1.2, §§2.9.1–2][hsa]

## Header and barrier representation

Every standard packet begins with a 16-bit header. The type is in bits 7:0,
the barrier bit is bit 8, acquire scope is bits 10:9, and release scope is
bits 12:11. Scope values are NONE = 0, AGENT = 1, and SYSTEM = 2. The barrier
bit delays the packet's active phase until preceding packets on the same queue
have completed. It does not select a cache or memory scope. [Header fields and
fence values][header]

| AND/OR byte offset | Field |
| --- | --- |
| 0 | 16-bit header: BARRIER_AND type 3 or BARRIER_OR type 5. |
| 2–7 | Reserved. |
| 8, 16, 24, 32, 40 | Five 64-bit dependency signal handles. |
| 48–55 | Reserved. |
| 56 | 64-bit completion signal handle; zero means no notification. |

An AND waits for every non-null dependency to equal zero; null entries are
already satisfied. An OR waits for at least one non-null dependency to equal
zero; null entries do not satisfy it. Reserved fields are zero in the defined
packet layout. [AND and OR definitions][layout]

For either barrier, dependency evaluation occurs before its acquire fence.
Later packets cannot become active until the barrier's active and completion
phases finish. Its own completion signal is atomically decremented after its
release fence. A kernel dispatch instead performs its acquire fence before its
active phase. These orderings are specified independently of the header
barrier bit. [HSA §§2.9.2, 2.9.8–9][hsa]

## Fence scope and observers

AGENT scope covers the participating work on one HSA agent. SYSTEM scope
includes host and other participating agents. On gfx942 this is not a choice
between an L1-only operation and a larger flush: the LLVM memory model
describes local and remote L2 paths within the target's agent model. [Fence
definitions][header] · [gfx942 memory model][llvm-gfx942]

A packet release fence covers the global stores of dispatches that completed
their active phase before the fence, across queues on that agent. An acquire
fence applies to dispatches on the agent that have not yet entered their
active phase. Execution dependencies determine which producer is in the former
set and which consumer is in the latter. Fence scopes alone do not create that
dependency. [HSA §3.3.8][hsa]

Kernargs have an additional publication rule. The host publishes initialized
argument storage with SYSTEM release before dispatch publication, and the
packet processor makes those arguments visible to the kernel independently of
the dispatch's acquire scope. Argument storage remains unchanged until
dispatch completion. This rule does not publish arbitrary payloads reached
through pointers in those arguments. [HSA §3.3.3.1][hsa]

An ordinary same-agent pipeline can use the following composition when the
payload mappings support the stated observers:

1. The host initializes inputs and kernargs, then performs SYSTEM publication.
2. Producer `P` uses SYSTEM acquire and AGENT release.
3. Consumer `C` has its header barrier bit set, uses AGENT acquire and SYSTEM
   release, and reads `P`'s output.
4. A trailing AND with its header barrier bit set, no dependencies, and a
   completion signal supplies a distinct terminal notification. NONE acquire
   and SYSTEM release suffice for this final packet's role.
5. The host acquires the terminal completion before reading outputs. Ring-slot
   retirement remains a separate prerequisite for rewriting the packet slots.

This sequence follows the packet phases and same-agent fence rules; the
terminal AND's wait for preceding completion protects both kernels even if
their own completion handles are null. The producer's SYSTEM ingress and
consumer's SYSTEM egress retain the host boundaries. [HSA §§2.9.2, 3.3.8][hsa]

CLR's scope choices are runtime policy, with explicit exceptions: its ordinary
dispatch path starts with AGENT scopes, widens acquire for the stated
gfx1200/gfx1201 optimization condition, and widens boundaries around copies or
external visibility. These callers do not make AGENT sufficient for every
mapping. ROCr's asynchronous-copy API requires system-level coherent buffers
and says that, in general, the sender and receiver need SYSTEM
release/acquire. [Dispatch policy][clr-scopes] · [Copy policy][clr-copy-scope]
· [Scope adjustment][clr-adjust-scope] · [External release][clr-release-scope]
· [Asynchronous-copy contract][rocr-dma-scope]

## Native signal storage and last consumers

The AMD native signal ABI is a 64-byte block aligned to 64 bytes. A USER
signal has signed 64-bit kind 1 at byte 0 and signed 64-bit value at byte 8.
The remaining block includes mailbox/event, timestamp, queue-related, and
reserved fields. A native handle identifies the ABI block; the surrounding
ROCr signal object has additional ownership and runtime metadata. [Native
layout][signal] · [ROCr object conversion][convert]

Initializing only the signal value does not create that surrounding runtime
object. Likewise, a host value store does not promise to clear all other
native fields. Rearming a retained signal updates its value under the
applicable atomic ordering after every prior consumer has finished; it does
not rewrite the entire ABI block while the packet processor may use it.
[Native host release store][host-signal] · [HSA signal lifetime][hsa]

An AND is guaranteed to complete once all dependencies are zero and remain
zero until that barrier completes. Observing each dependency reach zero at
unrelated times is insufficient if values can be changed again. For an OR,
completion does not identify which dependency won or establish completion of
the other producers. Their storage still has independent owners. These
distinctions are explicit in HSA §§2.9.8–9. [Specification][hsa]

## Fan-in and independent worksets

For two producer kernels `P0` and `P1`, each writes a disjoint intermediate
range and decrements its own signal after release. A consumer queue can
publish `AND(S0, S1, null, null, null)` followed by `C` before either producer
is submitted. NONE/NONE scopes on the AND can express only readiness; `C`'s
acquire then supplies the payload visibility boundary matching the producers'
releases. HSA scheduling requires dependencies not to occupy the resources
needed to make the prerequisite work progress. [HSA §§2.9.8, 2.10, 3.3.8][hsa]

The lifetime graph has more edges than the producer completion graph:

| Storage | Final user in this graph |
| --- | --- |
| Producer-only inputs and kernargs | The corresponding producer dispatch. |
| Intermediate payload | The consumer dispatch, after the producer. |
| Producer completion signals | The AND barrier, after their producers. |
| Consumer kernargs and output | The consumer dispatch. |
| Consumer completion signal | The host or later dependent packets observing it. |

Independent producer joins can establish producer-only ownership before
examining that storage. They cannot release intermediates or dependency
signals that the consumer still borrows. A terminal consumer join plus
retirement of all naming packet slots closes those uses. [HSA packet and
argument lifetimes, §§2.9 and 3.3.3.1][hsa]

The same reasoning applies to a host-gated consumer. If `AND(S, G)` waits on a
completed producer signal `S` and a host gate `G` that is still nonzero, the
intermediate, consumer arguments, dependency signals, and executable remain
owned by the pending graph. A separate workset can be reused after its own
terminal join without releasing any of those resources. Retaining a queue's
scratch backing through queue destruction is a further queue-level obligation;
it is not inferred from completion of one workset. See [private
storage](dispatch.md#private-storage).

## AMD BARRIER_VALUE epochs

AMD's vendor-specific BARRIER_VALUE packet uses format 2 and applies only to
AMD kernel-dispatch agents. It compares `(signal_value & mask)` with a signed
64-bit value using an HSA signal condition. A null dependency is satisfied.
[Extension contract][vendor]

| Byte offset | Field |
| --- | --- |
| 0 | Standard 16-bit header with vendor-specific type 0. |
| 2 | 8-bit AMD format, 2. |
| 3–7 | Reserved. |
| 8 | Dependency signal handle. |
| 16 | Signed 64-bit comparison value. |
| 24 | Signed 64-bit comparison mask. |
| 32 | 32-bit HSA condition; LT has value 2 in this enumeration. |
| 36–55 | Reserved. |
| 56 | Completion signal handle. |

CLR enables this path for `(major == 9 && minor == 0 && stepping == 10)` or
`(major == 9 && minor >= 4 && stepping in {0, 1, 2})` at the cited revision.
That is a consumer predicate, not a definition of every firmware implementing
the extension. Its packet builder supplies the native comparison fields.
[Selection predicate][clr-target] · [Builder and caller][clr-packet]

A descending positive epoch can avoid host rearming between dependent
dispatches. With a retained signal initially `E`, a producer completion
decrements it to `E - 1`; a consumer-side BARRIER_VALUE with full mask and `LT
E` waits for that transition. The consumer's acquire handles the payload
visibility boundary. Both queues' last users must finish before storage reuse,
and the next comparison must refer to the next intended epoch. Signed
comparison, finite range, and signal lifetime are part of this construction;
it supplies no rule for wraparound or resets. [Extension comparison][vendor] ·
[Completion and fence phases][hsa]

Return to [AQL](README.md) or the [primary source map](../../sources.md).

[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[header]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2825-L2949
[layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L3145-L3223
[llvm-gfx942]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11208-L11339
[clr-scopes]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2407-L2443
[clr-copy-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocblit.cpp#L581-L603
[clr-adjust-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1624-L1653
[clr-release-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2347-L2363
[rocr-dma-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2112
[signal]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_signal.h#L49-L78
[convert]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/signal.h#L295-L325
[host-signal]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/default_signal.cpp#L51-L75
[vendor]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L123-L229
[clr-target]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocsettings.cpp#L148-L153
[clr-packet]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2234-L2309
