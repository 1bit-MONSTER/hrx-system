// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/integer_relation.h"

#include "iree/testing/gtest.h"

namespace loom {
namespace {

struct RelationCase {
  loom_symbolic_integer_relation_t relation;
  loom_integer_order_outcome_bits_t outcomes;
  loom_symbolic_integer_relation_t swapped;
};

constexpr RelationCase kRelationCases[] = {
    {LOOM_SYMBOLIC_INTEGER_RELATION_EQ, LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL,
     LOOM_SYMBOLIC_INTEGER_RELATION_EQ},
    {LOOM_SYMBOLIC_INTEGER_RELATION_NE,
     LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
         LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER,
     LOOM_SYMBOLIC_INTEGER_RELATION_NE},
    {LOOM_SYMBOLIC_INTEGER_RELATION_LT, LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS,
     LOOM_SYMBOLIC_INTEGER_RELATION_GT},
    {LOOM_SYMBOLIC_INTEGER_RELATION_LE,
     LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS | LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL,
     LOOM_SYMBOLIC_INTEGER_RELATION_GE},
    {LOOM_SYMBOLIC_INTEGER_RELATION_GT, LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER,
     LOOM_SYMBOLIC_INTEGER_RELATION_LT},
    {LOOM_SYMBOLIC_INTEGER_RELATION_GE,
     LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL |
         LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER,
     LOOM_SYMBOLIC_INTEGER_RELATION_LE},
};

TEST(IntegerRelationTest, RoundTripsOutcomeSetsAndSwaps) {
  for (const RelationCase& test_case : kRelationCases) {
    EXPECT_EQ(loom_symbolic_integer_relation_outcomes(test_case.relation),
              test_case.outcomes);
    loom_symbolic_integer_relation_t recovered =
        LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
    ASSERT_TRUE(loom_symbolic_integer_relation_from_outcomes(test_case.outcomes,
                                                             &recovered));
    EXPECT_EQ(recovered, test_case.relation);
    EXPECT_EQ(loom_symbolic_integer_relation_swap(test_case.relation),
              test_case.swapped);
    loom_symbolic_integer_relation_t predicate_relation =
        LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
    const loom_predicate_kind_t predicate_kind =
        loom_symbolic_integer_relation_to_predicate_kind(test_case.relation);
    ASSERT_TRUE(loom_symbolic_integer_relation_from_predicate_kind(
        predicate_kind, &predicate_relation));
    EXPECT_EQ(predicate_relation, test_case.relation);
    EXPECT_EQ(loom_integer_order_outcomes_swap(test_case.outcomes),
              loom_symbolic_integer_relation_outcomes(test_case.swapped));
  }

  loom_symbolic_integer_relation_t unused = LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
  EXPECT_FALSE(loom_symbolic_integer_relation_from_outcomes(0, &unused));
  EXPECT_FALSE(loom_symbolic_integer_relation_from_outcomes(
      LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL, &unused));
  EXPECT_FALSE(loom_symbolic_integer_relation_from_predicate_kind(
      LOOM_PREDICATE_ULT, &unused));
  EXPECT_FALSE(loom_symbolic_integer_relation_from_predicate_kind(
      LOOM_PREDICATE_RANGE, &unused));
}

TEST(IntegerRelationTest, ResolvesEveryKnownAndQueriedPair) {
  for (const RelationCase& known : kRelationCases) {
    for (const RelationCase& queried : kRelationCases) {
      const auto matching = known.outcomes & queried.outcomes;
      const bool expected_known = matching == 0 || matching == known.outcomes;
      bool result = false;
      EXPECT_EQ(loom_symbolic_integer_relation_resolve(
                    known.outcomes, queried.relation, &result),
                expected_known);
      if (expected_known) {
        EXPECT_EQ(result, matching != 0);
      }

      bool implied_result = false;
      EXPECT_EQ(loom_symbolic_integer_relation_implies(
                    known.relation, queried.relation, &implied_result),
                expected_known);
      if (expected_known) {
        EXPECT_EQ(implied_result, matching != 0);
      }
    }
  }

  bool result = false;
  EXPECT_FALSE(loom_symbolic_integer_relation_resolve(
      0, LOOM_SYMBOLIC_INTEGER_RELATION_EQ, &result));
  EXPECT_FALSE(loom_symbolic_integer_relation_resolve(
      LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL, LOOM_SYMBOLIC_INTEGER_RELATION_EQ,
      &result));
}

TEST(IntegerRelationTest, ConvertsStrictOutcomes) {
  EXPECT_EQ(
      loom_integer_order_outcome_relation(LOOM_INTEGER_ORDER_OUTCOME_LESS),
      LOOM_SYMBOLIC_INTEGER_RELATION_LT);
  EXPECT_EQ(
      loom_integer_order_outcome_relation(LOOM_INTEGER_ORDER_OUTCOME_EQUAL),
      LOOM_SYMBOLIC_INTEGER_RELATION_EQ);
  EXPECT_EQ(
      loom_integer_order_outcome_relation(LOOM_INTEGER_ORDER_OUTCOME_GREATER),
      LOOM_SYMBOLIC_INTEGER_RELATION_GT);
  EXPECT_EQ(loom_integer_order_outcome_swap(LOOM_INTEGER_ORDER_OUTCOME_LESS),
            LOOM_INTEGER_ORDER_OUTCOME_GREATER);
  EXPECT_EQ(loom_integer_order_outcome_swap(LOOM_INTEGER_ORDER_OUTCOME_EQUAL),
            LOOM_INTEGER_ORDER_OUTCOME_EQUAL);
}

}  // namespace
}  // namespace loom
