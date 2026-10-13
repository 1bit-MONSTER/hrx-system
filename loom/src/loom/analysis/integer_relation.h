// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared integer relation vocabulary for symbolic proofs and edge facts.

#ifndef LOOM_ANALYSIS_INTEGER_RELATION_H_
#define LOOM_ANALYSIS_INTEGER_RELATION_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Integer relations over two scalar address/integer values.
typedef enum loom_symbolic_integer_relation_e {
  // left == right.
  LOOM_SYMBOLIC_INTEGER_RELATION_EQ = 0,
  // left != right.
  LOOM_SYMBOLIC_INTEGER_RELATION_NE = 1,
  // left < right.
  LOOM_SYMBOLIC_INTEGER_RELATION_LT = 2,
  // left <= right.
  LOOM_SYMBOLIC_INTEGER_RELATION_LE = 3,
  // left > right.
  LOOM_SYMBOLIC_INTEGER_RELATION_GT = 4,
  // left >= right.
  LOOM_SYMBOLIC_INTEGER_RELATION_GE = 5,
} loom_symbolic_integer_relation_t;

// Ordered comparison outcomes used to represent sets of possible relations.
typedef uint8_t loom_integer_order_outcome_t;
enum loom_integer_order_outcome_e {
  LOOM_INTEGER_ORDER_OUTCOME_LESS = 0,
  LOOM_INTEGER_ORDER_OUTCOME_EQUAL = 1,
  LOOM_INTEGER_ORDER_OUTCOME_GREATER = 2,
  LOOM_INTEGER_ORDER_OUTCOME_COUNT = 3,
};

// Bit set of ordered comparison outcomes.
typedef uint8_t loom_integer_order_outcome_bits_t;
enum loom_integer_order_outcome_bit_e {
  LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS = 1u << LOOM_INTEGER_ORDER_OUTCOME_LESS,
  LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL = 1u << LOOM_INTEGER_ORDER_OUTCOME_EQUAL,
  LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER =
      1u << LOOM_INTEGER_ORDER_OUTCOME_GREATER,
  LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL = LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
                                       LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL |
                                       LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER,
};

// Returns the logical negation of |relation|.
loom_symbolic_integer_relation_t loom_symbolic_integer_relation_invert(
    loom_symbolic_integer_relation_t relation);

// Returns the equivalent relation after swapping left and right operands.
loom_symbolic_integer_relation_t loom_symbolic_integer_relation_swap(
    loom_symbolic_integer_relation_t relation);

// Returns the exact set of less/equal/greater outcomes admitted by |relation|.
loom_integer_order_outcome_bits_t loom_symbolic_integer_relation_outcomes(
    loom_symbolic_integer_relation_t relation);

// Returns the relation represented by a nonempty, constrained outcome set.
// Empty and unconstrained sets return false without writing |out_relation|.
bool loom_symbolic_integer_relation_from_outcomes(
    loom_integer_order_outcome_bits_t outcomes,
    loom_symbolic_integer_relation_t* out_relation);

// Returns the equivalent outcome after swapping left and right operands.
loom_integer_order_outcome_t loom_integer_order_outcome_swap(
    loom_integer_order_outcome_t outcome);

// Returns the strict relation represented by one comparison outcome.
loom_symbolic_integer_relation_t loom_integer_order_outcome_relation(
    loom_integer_order_outcome_t outcome);

// Returns the equivalent outcome set after swapping left and right operands.
loom_integer_order_outcome_bits_t loom_integer_order_outcomes_swap(
    loom_integer_order_outcome_bits_t outcomes);

// Resolves |queried_relation| against a nonempty set of possible outcomes.
// Returns false when some outcomes satisfy the query and others do not.
bool loom_symbolic_integer_relation_resolve(
    loom_integer_order_outcome_bits_t outcomes,
    loom_symbolic_integer_relation_t queried_relation, bool* out_result);

// Attempts to prove |queried_relation| from one known |implied_relation| over
// the same ordered operands. Returns false when implication is unknown.
bool loom_symbolic_integer_relation_implies(
    loom_symbolic_integer_relation_t implied_relation,
    loom_symbolic_integer_relation_t queried_relation, bool* out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_INTEGER_RELATION_H_
