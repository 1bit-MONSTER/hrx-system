// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Recurrence equations shared by CFG and structured condition-loop facts.

#ifndef LOOM_UTIL_FACT_INDUCTION_H_
#define LOOM_UTIL_FACT_INDUCTION_H_

#include "loom/analysis/loop_domain.h"
#include "loom/ir/ir.h"
#include "loom/util/fact_recurrence.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_value_fact_table_t loom_value_fact_table_t;

// A recognized header-tested recurrence. Its inputs are invariant within the
// loop. The owning fact solver refreshes this equation after semantic edits;
// numeric queries consume current table entries, never cached counts.
typedef struct loom_value_fact_induction_t {
  // Header argument, or INVALID for an unrecognized or literal-false guard.
  loom_value_id_t value;
  // Value entering the header from outside the loop.
  loom_value_fact_recurrence_operand_t initial_value;
  // Invariant upper bound tested by the header guard.
  loom_value_fact_recurrence_operand_t upper_bound;
  // Invariant increment added by the backedge. An absent operand can still
  // establish zero trips from a false entry guard.
  loom_value_fact_recurrence_operand_t step;
  // Signedness and inclusivity of the guard.
  loom_loop_bound_flags_t bound_flags;
  // True when the header condition is a literal false. No backedge executes,
  // including after canonicalization removes the original comparison.
  bool exits_at_header;
} loom_value_fact_induction_t;

// Evaluates a retained recurrence against current facts and target carriers.
// Unknown or nonconstant inputs do not establish a numeric recurrence proof.
loom_loop_recurrence_facts_t loom_value_fact_induction_facts(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_value_fact_induction_t* induction);

// Recognizes the controlling recurrence at a verified LoopLike condition/body
// boundary. Called by the summary owner after computing declared SSA aliases;
// queries consume the retained equation instead of rediscovering its structure.
loom_value_fact_induction_t loom_value_fact_condition_loop_induction(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_loop_like_t loop);

// Builds additive state equations after the loop body has established its SSA
// identities. Counted loops use the body boundary; condition loops use the
// header boundary and its declared forwarding into the body.
iree_status_t loom_value_fact_loop_build_recurrences(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_loop_like_t loop, iree_arena_allocator_t* arena,
    loom_value_fact_recurrence_set_t* out_set);

// Publishes the current equation with the condition region's fact-scope
// lifetime. Recomputing a structured summary replaces the previous equation.
iree_status_t loom_value_fact_table_set_condition_induction(
    loom_value_fact_table_t* table, const loom_region_t* condition_region,
    loom_value_fact_induction_t induction);

// Returns the published equation, or NULL before this region is summarized.
const loom_value_fact_induction_t*
loom_value_fact_table_lookup_condition_induction(
    const loom_value_fact_table_t* table,
    const loom_region_t* condition_region);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_FACT_INDUCTION_H_
