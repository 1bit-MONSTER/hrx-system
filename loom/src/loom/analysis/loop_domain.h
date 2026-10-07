// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Dialect-independent loop-domain proofs. Callers supply bound facts or
// concrete integer recurrences; this component owns their numeric semantics.

#ifndef LOOM_ANALYSIS_LOOP_DOMAIN_H_
#define LOOM_ANALYSIS_LOOP_DOMAIN_H_

#include "iree/base/api.h"
#include "loom/ir/facts.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Half-open counted range [lower_bound, upper_bound) with a positive step.
// SSA value IDs keep the domain independent of the operation defining it.
typedef struct loom_loop_domain_t {
  // Inclusive lower bound of the counted loop domain.
  loom_value_id_t lower_bound;
  // Exclusive upper bound of the counted loop domain.
  loom_value_id_t upper_bound;
  // Positive step between consecutive induction values.
  loom_value_id_t step;
} loom_loop_domain_t;

// Returns true when every value admitted by the domain facts produces zero
// iterations. The proof requires a positive integer step and lower_bound >=
// upper_bound for the complete fact ranges.
bool loom_loop_domain_proven_empty(loom_value_facts_t lower_bound,
                                   loom_value_facts_t upper_bound,
                                   loom_value_facts_t step);

// Returns true when every value admitted by the domain facts produces at least
// one iteration. The proof requires a positive integer step and lower_bound <
// upper_bound for the complete fact ranges.
bool loom_loop_domain_proven_nonempty(loom_value_facts_t lower_bound,
                                      loom_value_facts_t upper_bound,
                                      loom_value_facts_t step);

// Upper-bound comparison semantics for a header-tested integer recurrence.
enum loom_loop_bound_flag_bits_e {
  // Unsigned exclusive comparison: induction value < upper bound.
  LOOM_LOOP_BOUND_NONE = 0,
  // Use signed two's-complement order for the induction value and bound.
  LOOM_LOOP_BOUND_SIGNED = 1u << 0,
  // Continue while the induction value <= bound instead of < bound.
  LOOM_LOOP_BOUND_INCLUSIVE = 1u << 1,
};
typedef uint8_t loom_loop_bound_flags_t;

// Counts body executions of a header-tested loop whose backedge adds |step| to
// the induction value. |bound_flags| selects signed/unsigned and exclusive/
// inclusive comparison with |upper_bound|. The initial value, bound, and step
// are raw carrier bits; only their low |bitwidth| bits participate. |bitwidth|
// is the verified carrier width in [1, 64].
//
// A false initial guard proves zero trips independently of the step. Nonempty
// loops require an increasing recurrence that reaches the exit without wrapping
// in the comparison's ordered carrier domain, including the terminal increment.
// This differs from the mathematical cardinality of a counted range: a finite
// range may still require an overflowing terminal increment in a lowered loop.
// Returns false when this proof cannot establish an exact count; it does not
// imply that the loop is infinite. |out_trip_count| is zero on failure.
bool loom_loop_domain_trip_count(loom_loop_bound_flags_t bound_flags,
                                 uint8_t bitwidth, uint64_t initial_value,
                                 uint64_t upper_bound, uint64_t step,
                                 uint64_t* out_trip_count);

// Source-integer facts for an additive recurrence over a finite loop domain.
typedef struct loom_loop_recurrence_facts_t {
  // Inclusive range of all header observations, including the exit state.
  // Unknown when the source-integer recurrence is unproven.
  loom_value_facts_t values;
  // Inclusive range observed on entry to the body, excluding the terminal
  // value. Unknown for zero trips or an unproven source-integer recurrence.
  loom_value_facts_t body_values;
  // Range of exit states, including zero-trip exits. Exact for an exact seed;
  // unknown when the source-integer recurrence is unproven.
  loom_value_facts_t exit_value;
  // Exact body execution count when trip_count_known is true; zero otherwise.
  uint64_t trip_count;
  // True when the exact body execution count is supplied or proven.
  bool trip_count_known;
} loom_loop_recurrence_facts_t;

// Evaluates initial_values + iteration * step for the supplied exact trip
// count. |initial_values| contains integer range and divisibility facts. The
// header includes iterations [0, trip_count], the body [0, trip_count), and
// the exit only trip_count. A zero-trip body has unknown facts.
//
// Returns true when every represented initial value and every resulting state
// fit the mathematical signed i64 source domain. On failure all three ranges
// are unknown; the supplied trip count remains known. The result retains only
// numeric range and divisibility facts, not context-local extensions or
// distribution. This proof neither establishes a loop's trip count nor selects
// overflow semantics. Target-carrier representability remains with the caller.
bool loom_loop_domain_additive_recurrence_facts(
    loom_value_facts_t initial_values, int64_t step, uint64_t trip_count,
    loom_loop_recurrence_facts_t* out_facts);

// Proves the same recurrence as trip_count, retaining a source-integer range
// when its initial value and positive increments remain representable in the
// signed carrier. An exact count alone does not imply such a range: unsigned
// order can cross the sign bit, and a modular increment can be negative in the
// source representation. A zero-trip header and exit contain only the initial
// value; no body range is established.
loom_loop_recurrence_facts_t loom_loop_domain_recurrence_facts(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth,
    int64_t initial_value, int64_t upper_bound, int64_t step);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_ANALYSIS_LOOP_DOMAIN_H_
