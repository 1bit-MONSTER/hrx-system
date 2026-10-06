# Semantic correctness corpus

This tree owns target-neutral Loom programs that establish executable language
semantics. Each `.loom` file keeps the subject operations together with its
`check.case` inputs and expected results. Compiler targets and execution devices
remain outside these source packages.

Each semantic package exports an exhaustive source inventory from
`manifest.bzl` through `loom_corpus_sources`. The root `catalog.bzl` composes
those manifests into `LOOM_CORPUS`; it contains no target policy. A source-only
change consequently invalidates the independent build or execution action for
that program rather than a monolithic corpus module. Cases and scenario trials
within one program remain batched.

Target packages instantiate the shared catalog under
`loom/target/<family>/test/corpus/`. Those packages own compiler profiles,
execution profiles, and target support status. A repairable compiler gap is an
exact `<source>:@<root>` diagnostic xfail. Sources whose complete root set
xfails also use `all_roots_xfail`, which makes a newly added or newly supported
root fail qualification until the target matrix is updated. Whole-source
exclusions represent semantic incompatibilities that cannot be implemented on
the target, such as an execution model with no equivalent barrier contract.

The VM corpus executes every enrolled source independently through
`iree-test-loom`. Compile-only targets emit the deployable artifact and detailed
compile report for every supported root set; diagnostic probes remain batched
per source and profile.

Inputs cross a callable boundary so execution exercises compiled operations
instead of specializing the subject from test values. Expectations use
independently derived values or precisely justified identities. Signed zeros,
NaNs, narrow formats, and packed fields use bitwise comparisons when their
payload is part of the contract. Approximate operations carry their authored
accuracy policy.

A case earns its place through an observable distinction: a boundary value,
type combination, alias relationship, control-flow path, or interaction between
operations. Sample count alone does not establish language coverage. Unsupported
valid programs remain shared semantic evidence while their target-owned xfails
identify the compiler or execution-adapter work required to support them.
