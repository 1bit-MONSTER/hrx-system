# Loom Correctness Corpus

This tree is the target-neutral semantic correctness corpus. A source file owns
one coherent language or execution family; the directory hierarchy describes
that semantic ownership. It does not classify an execution ABI. Functions,
kernels, pipelines, and command programs can all be scenario subjects on every
target whose execution profile implements their semantics.

`check.scenario` is the primary corpus harness. New corpus sources define
scenarios, and new coverage extends the existing source that owns the semantic
family. A scenario generates runtime inputs, invokes the subject and an
independent oracle through `check.compare`, and checks the observable results.
Values crossing the subject boundary remain runtime values so target
compilation cannot specialize on generated inputs.

Each `manifest.bzl` lists scenario sources in one `srcs` inventory shared by
compilation and execution. A corpus source without an authored scenario is
invalid; direct `check.case` records belong in focused dialect or runner tests,
not the correctness corpus.

Target packages execute the shared scenario catalog. A missing implementation
is an xfail on the exact scenario and diagnostic so an implementation or a
diagnostic change is visible. Exclusions represent permanent semantic
incompatibility, such as a target with no kernel execution model. Incomplete
lowering and runtime support remain visible as exact xfails.

Small compiler-shape assertions live in focused `.loom-test` files beside the
owning compiler subsystem. The correctness corpus consists of authored programs
with oracle comparisons, while normal build outputs and compile reports expose
their target lowering. This boundary excludes generated `.loom` programs,
per-target copies, large textual lowering expectations, and source dependencies
on `.loom-test` fixtures.
