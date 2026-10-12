# Source-Low Corpus

This directory contains standalone source programs used as explicit compiler
or integration-test inputs. A `.loom-test` suffix selects the multi-case
container format; package registration determines whether that container is a
test. Source files without inline expectations are data, not parser/printer
roundtrip tests.

Each build registration owns the target profiles against which its source must
compile. Successful compilation provides target-coverage signal but is not a
correctness assertion by itself. Target-specific lowering, report, and
diagnostic fixtures keep their source beside their exact expectations under the
owning target or pass. Missing lowering support belongs in work tracking rather
than expected errors copied across positive programs.

A case containing several functions has one public function or kernel entry and
private helpers. The entry identifies the case and receives the requested
compiler profile; call-graph specialization supplies helper targets in
compiler-owned function versions. Compiler options preserve source visibility.

Shared analysis and report semantics have focused tests beside the owning
analysis or pass. Backend-independent tests cover behavior such as loop
execution counts and nested emission accounting. Corpus programs here exercise
end-to-end compilation or provide data to integration tests without duplicating
target-specific assertions.

Target-specific diagnostics and expected failures stay beside the target or
pass that owns the diagnostic vocabulary. Those fixtures can assert the exact
structured error or remark without forcing other targets to inherit unrelated
failure semantics.
