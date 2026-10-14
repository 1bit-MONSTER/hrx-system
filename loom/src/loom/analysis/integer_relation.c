// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/integer_relation.h"

// Both families intentionally share ordinals so conversion remains a range
// check and cast. Prove each against the common numeric contract instead of
// comparing the enum domains directly, which MSVC diagnoses as C5287.
#define LOOM_ASSERT_SIGNED_RELATION_ORDINAL(name, ordinal)             \
  static_assert((int)LOOM_PREDICATE_##name == ordinal,                 \
                "signed predicate ordinal must remain stable");        \
  static_assert((int)LOOM_SYMBOLIC_INTEGER_RELATION_##name == ordinal, \
                "symbolic integer relation ordinal must remain stable")
LOOM_ASSERT_SIGNED_RELATION_ORDINAL(EQ, 0);
LOOM_ASSERT_SIGNED_RELATION_ORDINAL(NE, 1);
LOOM_ASSERT_SIGNED_RELATION_ORDINAL(LT, 2);
LOOM_ASSERT_SIGNED_RELATION_ORDINAL(LE, 3);
LOOM_ASSERT_SIGNED_RELATION_ORDINAL(GT, 4);
LOOM_ASSERT_SIGNED_RELATION_ORDINAL(GE, 5);
#undef LOOM_ASSERT_SIGNED_RELATION_ORDINAL

static const loom_integer_order_outcome_bits_t
    loom_symbolic_integer_relation_outcome_table[] = {
        [LOOM_SYMBOLIC_INTEGER_RELATION_EQ] =
            LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL,
        [LOOM_SYMBOLIC_INTEGER_RELATION_NE] =
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER,
        [LOOM_SYMBOLIC_INTEGER_RELATION_LT] =
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS,
        [LOOM_SYMBOLIC_INTEGER_RELATION_LE] =
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
            LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL,
        [LOOM_SYMBOLIC_INTEGER_RELATION_GT] =
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER,
        [LOOM_SYMBOLIC_INTEGER_RELATION_GE] =
            LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL |
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER,
};

loom_symbolic_integer_relation_t loom_symbolic_integer_relation_invert(
    loom_symbolic_integer_relation_t relation) {
  switch (relation) {
    case LOOM_SYMBOLIC_INTEGER_RELATION_EQ:
      return LOOM_SYMBOLIC_INTEGER_RELATION_NE;
    case LOOM_SYMBOLIC_INTEGER_RELATION_NE:
      return LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
    case LOOM_SYMBOLIC_INTEGER_RELATION_LT:
      return LOOM_SYMBOLIC_INTEGER_RELATION_GE;
    case LOOM_SYMBOLIC_INTEGER_RELATION_LE:
      return LOOM_SYMBOLIC_INTEGER_RELATION_GT;
    case LOOM_SYMBOLIC_INTEGER_RELATION_GT:
      return LOOM_SYMBOLIC_INTEGER_RELATION_LE;
    case LOOM_SYMBOLIC_INTEGER_RELATION_GE:
      return LOOM_SYMBOLIC_INTEGER_RELATION_LT;
    default:
      return relation;
  }
}

loom_symbolic_integer_relation_t loom_symbolic_integer_relation_swap(
    loom_symbolic_integer_relation_t relation) {
  switch (relation) {
    case LOOM_SYMBOLIC_INTEGER_RELATION_LT:
      return LOOM_SYMBOLIC_INTEGER_RELATION_GT;
    case LOOM_SYMBOLIC_INTEGER_RELATION_LE:
      return LOOM_SYMBOLIC_INTEGER_RELATION_GE;
    case LOOM_SYMBOLIC_INTEGER_RELATION_GT:
      return LOOM_SYMBOLIC_INTEGER_RELATION_LT;
    case LOOM_SYMBOLIC_INTEGER_RELATION_GE:
      return LOOM_SYMBOLIC_INTEGER_RELATION_LE;
    case LOOM_SYMBOLIC_INTEGER_RELATION_EQ:
    case LOOM_SYMBOLIC_INTEGER_RELATION_NE:
    default:
      return relation;
  }
}

bool loom_symbolic_integer_relation_from_predicate_kind(
    loom_predicate_kind_t predicate_kind,
    loom_symbolic_integer_relation_t* out_relation) {
  if (predicate_kind > LOOM_PREDICATE_GE) {
    return false;
  }
  *out_relation = (loom_symbolic_integer_relation_t)predicate_kind;
  return true;
}

loom_predicate_kind_t loom_symbolic_integer_relation_to_predicate_kind(
    loom_symbolic_integer_relation_t relation) {
  return (loom_predicate_kind_t)relation;
}

loom_integer_order_outcome_bits_t loom_symbolic_integer_relation_outcomes(
    loom_symbolic_integer_relation_t relation) {
  if ((unsigned)relation >=
      sizeof(loom_symbolic_integer_relation_outcome_table) /
          sizeof(loom_symbolic_integer_relation_outcome_table[0])) {
    return 0;
  }
  return loom_symbolic_integer_relation_outcome_table[relation];
}

bool loom_symbolic_integer_relation_from_outcomes(
    loom_integer_order_outcome_bits_t outcomes,
    loom_symbolic_integer_relation_t* out_relation) {
  switch (outcomes) {
    case LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
      return true;
    case LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
        LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_NE;
      return true;
    case LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_LT;
      return true;
    case LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
        LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_LE;
      return true;
    case LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_GT;
      return true;
    case LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL |
        LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_GE;
      return true;
    default:
      return false;
  }
}

loom_integer_order_outcome_t loom_integer_order_outcome_swap(
    loom_integer_order_outcome_t outcome) {
  if (outcome == LOOM_INTEGER_ORDER_OUTCOME_LESS) {
    return LOOM_INTEGER_ORDER_OUTCOME_GREATER;
  }
  if (outcome == LOOM_INTEGER_ORDER_OUTCOME_GREATER) {
    return LOOM_INTEGER_ORDER_OUTCOME_LESS;
  }
  return outcome;
}

loom_symbolic_integer_relation_t loom_integer_order_outcome_relation(
    loom_integer_order_outcome_t outcome) {
  switch (outcome) {
    case LOOM_INTEGER_ORDER_OUTCOME_LESS:
      return LOOM_SYMBOLIC_INTEGER_RELATION_LT;
    case LOOM_INTEGER_ORDER_OUTCOME_EQUAL:
      return LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
    case LOOM_INTEGER_ORDER_OUTCOME_GREATER:
      return LOOM_SYMBOLIC_INTEGER_RELATION_GT;
    default:
      return LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
  }
}

loom_integer_order_outcome_bits_t loom_integer_order_outcomes_swap(
    loom_integer_order_outcome_bits_t outcomes) {
  loom_integer_order_outcome_bits_t swapped =
      outcomes & LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL;
  if (outcomes & LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS) {
    swapped |= LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER;
  }
  if (outcomes & LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER) {
    swapped |= LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS;
  }
  return swapped;
}

bool loom_symbolic_integer_relation_resolve(
    loom_integer_order_outcome_bits_t outcomes,
    loom_symbolic_integer_relation_t queried_relation, bool* out_result) {
  outcomes &= LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL;
  if (outcomes == 0) {
    return false;
  }
  const loom_integer_order_outcome_bits_t queried_outcomes =
      loom_symbolic_integer_relation_outcomes(queried_relation);
  const loom_integer_order_outcome_bits_t matching =
      outcomes & queried_outcomes;
  if (matching != 0 && matching != outcomes) {
    return false;
  }
  *out_result = matching != 0;
  return true;
}

bool loom_symbolic_integer_relation_implies(
    loom_symbolic_integer_relation_t implied_relation,
    loom_symbolic_integer_relation_t queried_relation, bool* out_result) {
  return loom_symbolic_integer_relation_resolve(
      loom_symbolic_integer_relation_outcomes(implied_relation),
      queried_relation, out_result);
}
