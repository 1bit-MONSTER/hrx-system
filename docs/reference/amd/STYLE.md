# Writing the AMD hardware reference

The reader is implementing a native programming model or investigating how a
hardware mechanism works. A chapter supplies enough information to understand
its representation, preconditions, execution, and interaction with surrounding
work. Its explanation stands independently of any particular client library,
application, or validation framework.

## Organization

The reference is organized by hardware family and mechanism. GPU command
processors and transfer engines have separate PM4, AQL, and SDMA chapters.
AI Engine array chapters describe tile execution, memory and DMA, interconnects,
firmware control, and observation. Shared topics belong at the lowest level
that owns the same semantics: a GPU clock discussion can serve several GPU
engines, while an array timer retains its own clock and reset contract.
The interop section composes CPU, GPU and NPU mechanisms across directed
memory edges. Shared-allocation transports and whole-pipeline ownership live
there; individual engine and array chapters retain their native operations.

An index explains how the mechanisms fit together and links to their chapters.
An operation chapter owns one coherent group of representations and invariants.
A recipe composes those operations across actors. Architecture comparisons sit
beside the affected mechanism, where their consequences can be explained.

File names describe the mechanism with ordinary words: `dispatch.md`,
`command-buffers.md`, `cache.md`, or `observability.md`. Directories group
hardware families and engines. A filename or heading remains useful as an
operation gains additional architecture variants.

## Chapter structure

The opening paragraph explains what the mechanism does, which actor executes
it, and the observable result. The reader reaches that explanation before any
source archaeology. The following subjects give operation chapters a common
shape; closely related subjects can share a section when that improves clarity.

| Subject | Information supplied |
| --- | --- |
| Applicability | Architecture or engine IP, firmware conditions, native transport, and relevant operating modes. A source-selected runtime path names its actual predicate. |
| Representation | Packet or register fields, widths, offsets, units, alignment, valid values, and reserved fields for the stated revision. |
| Execution and ordering | What starts the operation, what it waits for, when later work can proceed, and what each completion observation establishes. |
| Memory and lifetime | Address spaces, mapping/cache attributes, visibility operations, borrowed storage, and the final user of each resource. |
| Programming sequence | A complete actor flow using the mechanism with its prerequisites and completion/reuse boundary. |
| Architecture and transport differences | Changed layouts, firmware workarounds, native access rules, and precisely attributed runtime choices. |

A chapter containing several related protocols uses these subjects within each
protocol. For example, an atomic decrement and a command-retirement increment
can share an applicability overview while retaining separate representations,
participants, and storage lifetimes. Sections contain actual information;
headings alone imply no behavior or architecture coverage.

## Evidence and applicability

Claims identify the layer that supplies their meaning:

| Layer | How the claim reads |
| --- | --- |
| Architecture or ABI specification | The named revision defines the field, operation, or memory-model rule. |
| Firmware protocol | The packet processor or controller consumes the stated representation under the identified firmware/native interface. |
| Native driver | The cited implementation establishes a mapping, queue mode, resource owner, or submission wrapper for that transport. |
| Runtime policy | The named consumer selects a particular operation, limit, scope, or workaround under an explicit predicate. |
| Inference | The conclusion and its premises are stated together, including the information that the sources leave unresolved. |

Packet definitions establish representation. Builders establish emitted values.
Callers establish the conditions under which those builders run. Resource
owners establish publication, completion, and reuse. A programming sequence
connects these pieces rather than assigning all four meanings to a header.

Compiler target names, physical IP revisions, PCI identities, firmware versions,
and partition topology keep their native names and units. Numeric ordering of
compiler targets supplies only the predicate used by a cited source; it does
not establish a general generation boundary. A runtime maximum and a packet's
representable maximum are recorded separately.

An unresolved disagreement names the exact field or behavior, the sources that
differ, and the applicability information needed to distinguish them. The prose
states the facts each source establishes. An observed workaround retains the
responsible architecture, firmware, driver, or runtime condition.

## Fields, quantities, and terminology

Packet tables identify both the containing word and the bit range. Byte offsets,
DWORD offsets, byte counts, DWORD counts, count-minus-one fields, and addresses
are explicit. A DWORD is 32 bits. Hexadecimal constants use a `0x` prefix;
field and symbol names use code formatting.

The first field or register table includes each full native spelling, such as
`COMPUTE_PGM_RSRC2`, even when nearby prose uses a shorter name. Alternate source
spellings appear together, such as `COPY_LINEAR_SUBWIN` and `COPY_LINEAR_RECT`.
Index descriptions include the corresponding packet or register names so an
exact symbol search reaches both the mechanism and its navigation entry.

Alignment and size constraints identify their owner. A compiler entry-point
alignment, a command-buffer alignment, and the alignment chosen for a runtime
allocation are distinct facts even when their numeric values coincide.
Reserved operands are described for the cited layout. An implementation's
zero-initialization is attributed to that implementation.

Memory descriptions name placement, access path, cache policy, and observer
scope separately. System memory and a SYSTEM-scope fence are different
concepts. A coherent mapping still needs an execution dependency. Atomicity,
visibility, ordering, completion, notification, and storage retirement each
retain their own meaning.

## Programming sequences

A sequence identifies the producer, consumer, command processor or controller,
and any participating host. It explains:

1. The resources and native mappings that exist before publication.
2. How commands, arguments, code, and input data become visible to their users.
3. The operations and dependencies that transfer control and payload ownership.
4. The completion observation for each independent user.
5. When commands, signals, executable code, and payload storage can be reused.

Symbolic addresses and values make the native operation visible. Concrete
numbers illustrate an encoding, granularity, or boundary when they contribute
to the explanation. Example dimensions remain examples; hardware limits have
their own source and applicability.

Pseudocode names native operations or explicit abstract actions such as
publishing a ring extent or acquiring a completed result. A complete sequence
includes the native work surrounding a packet when that work supplies cache
maintenance or retirement. A runtime-owned trailer is attributed to its
transport instead of being silently inherited by another queue model.

## Timing and observation

A timestamp has a clock domain, units, valid width, epoch/reset behavior, and
an execution point. A counter has an event definition, selector, instance,
attribution scope, enable/reset/read/stop sequence, and overflow behavior.
Conversions and aggregation preserve those meanings.

Timing examples identify the measured interval: host publication, observed
completion, device execution, transfer, or handoff. Synchronization and
instrumentation contribute to that interval explicitly. Frequency conversion
and cross-clock correlation have source-backed inputs and uncertainty.

## Sources and navigation

Citations sit beside the claim or table they support. Source-code links use an
immutable revision and the smallest useful symbol or line range. A public
specification names its revision and section or table. The source map records
the common revisions and each project's role; the chapter still supplies the
specific evidence for its claims.

Reference-style Markdown links keep long source URLs out of the explanation.
Link labels describe the relevant definition, builder, caller, or owner.
Definitions at the end of a page contain the URLs. A cluster of sources is
accompanied by the relationship among them: agreement, complementary evidence,
shared generated definitions, or a concrete disagreement.

Local navigation stays within this reference tree. External navigation points
to primary hardware, firmware, compiler, driver, or runtime sources. Readers
can copy the tree into another repository without acquiring a dependency on
the repository that hosted it.

## Prose and layout

Prose describes the mechanism in the present tense, with source revision and
applicability carrying version-specific meaning. Each paragraph develops one
relationship. Terms such as “supports,” “requires,” and “completes” identify
the actor and conditions that make the statement true.

Tables serve field layouts, applicability comparisons, and parallel actor
flows. Numbered lists describe ordered programming steps. Diagrams clarify
data movement or dependencies when prose alone would obscure them. Short
chapters can express the full structure in a few sections; complex chapters
expand the mechanisms without adding a running development history.
