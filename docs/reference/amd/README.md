# AMD hardware reference

This reference describes AMD hardware and firmware programming: command and
register representations, execution ordering, memory visibility, resource
lifetime, and architecture-specific behavior. Native driver interfaces and
upstream runtime implementations supply the context needed to use those
mechanisms on a particular platform.

| Area | Contents |
| --- | --- |
| [GPU](gpu/README.md) | Command processors, transfer engines, and their memory and signaling protocols. |
| [XDNA](xdna/README.md) | AI Engine array execution, firmware commands, timers, counters, and trace. |
| [Sources](sources.md) | Architecture specifications, immutable implementation revisions, and the role of each source. |
| [Writing guide](STYLE.md) | Chapter structure, terminology, evidence, and citation conventions. |

Each mechanism carries its own architecture, firmware, transport, and memory
conditions. A compiler target, physical engine revision, operating-system
interface, and runtime policy describe different parts of that applicability.
Where sources disagree, the affected fields and source-specific interpretations
remain explicit.
