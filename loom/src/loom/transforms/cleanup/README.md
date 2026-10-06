# Canonicalization and source combines

`canonicalize` applies universal simplifications throughout the compilation
pipeline. It folds constants, removes dead operations, propagates value and
type facts, and applies operation canonicalization rules. Cleanup after
legalization preserves the representation chosen by that legalization.

`combine` runs the same simplifications and adds source representation
combines. Adjacent scalar loads can become a vector load; scalar table
extracts followed by vector construction can become `vector.table.lookup`.
Ordered scalar rounding and conversion chains can become elementwise vector
operations. Integer comparisons selecting their own operands become min/max.
These operations may need target legalization, so `combine` belongs before that
boundary. The source pipeline and C++ importer CLI select this pass.

For example, a pipeline can use:

```text
combine,cse,target-legalize{mode=eager},canonicalize
```

The final cleanup can simplify a scalarized lookup without reconstructing the
operation that legalization decomposed. Both passes accept `max-iterations`;
pattern selection follows the pass contract.

The pass facades share the reusable canonicalizer in `canonicalizer.h`. Its
driver borrows the caller's value-fact owner and maintains one greedy worklist,
symbolic-expression context, and type-propagation session. The combine facade
selects additional patterns that receive those existing contexts. Direct
callers such as whole-program boundary refinement use ordinary canonicalization
and retain their seed-fact and callable-boundary contracts.

Compiler compositions select kind-indexed pattern providers for four ordered
phases. Region-initialization patterns run once in region preorder, universal
pre-fold and post-type patterns participate in the fixed-point worklist, and
source combines run only at the explicit source boundary. SCF branch-edge facts
use both the initialization and pre-fold phases: the initial walk establishes
outer branch facts before nested roots, while exact SCF-root dispatch maintains
new or changed branches without rescanning the whole function on every
iteration. Direct canonicalizer users project the same universal phase set from
their cleanup capability, so nested passes do not silently lose configured
dialect behavior.

The same compiler composition supplies the special-value policy used by the
generic driver. Poison and empty propagation remain universal trait/interface
logic, while the policy identifies supported values and types and builds the
selected dialect's poison, empty, and constant operations. A minimal compiler
can omit that policy, leaving those materializations disabled instead of
implicitly selecting concrete operation builders.
