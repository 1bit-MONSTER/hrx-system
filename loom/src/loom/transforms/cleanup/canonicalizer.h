// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_CLEANUP_CANONICALIZER_H_
#define LOOM_TRANSFORMS_CLEANUP_CANONICALIZER_H_

#include "loom/analysis/symbolic_expr.h"
#include "loom/pass/types.h"
#include "loom/rewrite/pattern_registry.h"
#include "loom/rewrite/type_propagation.h"
#include "loom/transforms/cleanup/fact_refinement_policy.h"
#include "loom/transforms/cleanup/special_value_policy.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_canonicalizer_state_t loom_canonicalizer_state_t;
typedef struct loom_cleanup_pattern_registry_t loom_cleanup_pattern_registry_t;

//===----------------------------------------------------------------------===//
// Canonicalizer driver
//===----------------------------------------------------------------------===//

// Default maximum number of canonicalizer fixed-point iterations.
#define LOOM_CANONICALIZER_DEFAULT_MAX_ITERATIONS 10

// Indexed pattern registries selected for one canonicalizer run. Each registry
// is optional and runs at the named ordering point in the shared fixed-point
// driver.
typedef struct loom_canonicalizer_pattern_registries_t {
  // Patterns applied once in region preorder before worklist processing.
  const loom_rewrite_pattern_registry_t* region_initialization;
  // Patterns applied after mini-DCE and before built-in poison/fold rules.
  const loom_rewrite_pattern_registry_t* pre_fold;
  // Patterns applied after type propagation and before symbolic cleanup.
  const loom_rewrite_pattern_registry_t* post_type;
  // Patterns applied after structural op canonicalization.
  const loom_rewrite_pattern_registry_t* post_canonicalization;
} loom_canonicalizer_pattern_registries_t;

// Projects universal cleanup phases from |registry| for a canonicalizer run.
// Source-combine patterns remain excluded because they are legal only at the
// explicit source-combine pipeline boundary. NULL returns an empty selection.
loom_canonicalizer_pattern_registries_t
loom_canonicalizer_pattern_registries_from_cleanup_registry(
    const loom_cleanup_pattern_registry_t* registry);

// Canonicalizer driver options. Zero-initialized options use defaults.
typedef struct loom_canonicalizer_options_t {
  // Maximum number of fixed-point iterations. Zero selects the default.
  uint32_t max_iterations;

  // Optional phase-specific patterns sharing the ordinary fixed-point driver.
  loom_canonicalizer_pattern_registries_t patterns;

  // Optional immutable target facts used by target-sensitive fact inference.
  const loom_target_facts_t* target_facts;

  // Borrowed math policy for this target. NULL retains optional contraction.
  const struct loom_target_math_policy_t* math_policy;

  // Optional function/region-local seeds cloned before the initial analysis.
  // The caller selects values in this scope; other entries in the source table
  // are not imported. Extension payloads are re-interned, so the seeds may come
  // from a different fact context. The view is borrowed for the run; target
  // scope is supplied independently by target_facts.
  loom_value_fact_table_view_t seed_facts;

  // Optional whole-module owner permitting callable boundary type changes.
  // Borrowed for the run; without an owner, callable types remain fixed.
  loom_type_propagator_boundary_callback_t refine_boundary;
} loom_canonicalizer_options_t;

// Summary of one canonicalizer function run.
typedef struct loom_canonicalizer_result_t {
  // True if the driver changed IR by erasing, replacing, moving, creating, or
  // otherwise mutating an operation/value.
  bool changed;

  // True if incremental fact recomputation changed at least one stored value
  // fact during rewriting.
  bool facts_changed;

  // True if a rewrite changed at least one value type.
  bool types_changed;

  // Conservative boundary invalidation bit. True when summaries derived from
  // this function's externally visible values may need recomputation.
  bool boundary_maybe_changed;

  // Number of ops modified by canonicalization.
  int64_t ops_modified;

  // Number of type propagation candidate closures rejected as inconsistent.
  int64_t type_propagation_conflicts;

  // Number of repeated rejected candidates skipped within an iteration.
  int64_t type_propagation_rejection_cache_hits;
} loom_canonicalizer_result_t;

// Stateful canonicalizer that can be driven by a pass or by whole-program
// refinement. The driver owns a resettable scratch arena for one function run;
// the caller owns |parent_arena| and the IR module arena.
typedef struct loom_canonicalizer_t {
  // Module being transformed.
  loom_module_t* module;

  // Caller-owned reusable value-fact storage used for function/region analysis.
  loom_pass_value_fact_owner_t* value_facts;

  // Compiler-selected special-value policy, or NULL to disable materialization.
  const loom_cleanup_special_value_policy_t* special_value_policy;

  // Compiler-selected fact-refinement materializers, or NULL to disable
  // relation preservation across exact folding.
  const loom_fact_refinement_policy_t* fact_refinement_policy;

  // Parent arena whose block pool backs the resettable scratch arena.
  iree_arena_allocator_t* parent_arena;

  // Reset before each public run; owns the rewriter worklist and
  // symbolic-expression scratch state for that run.
  iree_arena_allocator_t scratch_arena;

  // True after scratch_arena has been initialized and before deinitialize.
  bool scratch_arena_initialized;

  // Private driver state allocated from parent_arena. Holds the active rewriter
  // and any future canonicalizer-local state without exposing implementation
  // details through this header.
  loom_canonicalizer_state_t* state;
} loom_canonicalizer_t;

// Initializes a canonicalizer over |module|. The two policies supply
// compiler-selected builders and may be NULL to disable their respective
// materializations. |parent_arena| is not used for bulk scratch allocations
// directly; its block pool backs a nested arena that is reset for each run.
iree_status_t loom_canonicalizer_initialize(
    loom_module_t* module, iree_arena_allocator_t* parent_arena,
    loom_pass_value_fact_owner_t* value_facts,
    const loom_cleanup_special_value_policy_t* special_value_policy,
    const loom_fact_refinement_policy_t* fact_refinement_policy,
    loom_canonicalizer_t* out_canonicalizer);

// Releases transient worklist state and returns scratch blocks to the parent
// arena's block pool. Does not modify the module.
void loom_canonicalizer_deinitialize(loom_canonicalizer_t* canonicalizer);

// Runs canonicalization on an explicit region tree. |function| supplies the
// logical function context for value-fact inference and may be empty for
// detached regions. |parent_op| owns the root entry block arguments when
// provided.
iree_status_t loom_canonicalizer_run_region(
    loom_canonicalizer_t* canonicalizer, loom_func_like_t function,
    loom_region_t* region, loom_op_t* parent_op,
    const loom_canonicalizer_options_t* options,
    loom_canonicalizer_result_t* out_result);

// Runs canonicalization on a function-like op's root regions. Facts are
// computed once across the function; non-body regions canonicalize before the
// body so their incremental updates are visible to body canonicalization.
iree_status_t loom_canonicalizer_run_function(
    loom_canonicalizer_t* canonicalizer, loom_func_like_t function,
    const loom_canonicalizer_options_t* options,
    loom_canonicalizer_result_t* out_result);

// Returns the caller-owned fact table incrementally maintained by the most
// recent run. A later run may replace the table. Deinitializing the
// canonicalizer detaches the table without changing its owner-managed scope.
const loom_value_fact_table_t* loom_canonicalizer_fact_table(
    const loom_canonicalizer_t* canonicalizer);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CLEANUP_CANONICALIZER_H_
