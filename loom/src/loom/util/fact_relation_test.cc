// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_relation.h"

#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/util/fact_table.h"
#include "loom/util/predicate_facts.h"

namespace loom {
namespace {

class FactRelationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = loom_index_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_INDEX, vtables, static_cast<uint16_t>(count)));
    vtables = loom_scalar_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_SCALAR, vtables, static_cast<uint16_t>(count)));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("relations"), &pool_,
                                        nullptr, iree_allocator_system(),
                                        &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
    IREE_ASSERT_OK(loom_value_fact_table_initialize(&table_, &arena_, 4));
    for (loom_value_id_t& input : inputs_) {
      IREE_ASSERT_OK(loom_builder_define_value(&builder_, type_, &input));
      IREE_ASSERT_OK(loom_value_fact_table_define(&table_, input,
                                                  loom_value_facts_unknown()));
    }
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  iree_status_t BuildIdentity(loom_value_id_t input, loom_op_t** out_op) {
    return loom_index_assume_build(&builder_, &input, 1, nullptr, 0, &type_, 1,
                                   LOOM_LOCATION_UNKNOWN, out_op);
  }

  iree_status_t BuildAssume(const std::vector<loom_value_id_t>& values,
                            const std::vector<loom_predicate_t>& predicates,
                            loom_op_t** out_op) {
    std::vector<loom_type_t> result_types(values.size(), type_);
    return loom_index_assume_build(&builder_, values.data(), values.size(),
                                   predicates.data(), predicates.size(),
                                   result_types.data(), result_types.size(),
                                   LOOM_LOCATION_UNKNOWN, out_op);
  }

  static loom_predicate_t RelationPredicate(loom_predicate_kind_t kind,
                                            loom_value_id_t lhs,
                                            loom_value_id_t rhs) {
    return {
        .kind = kind,
        .arg_count = 2,
        .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_VALUE,
                     LOOM_PRED_ARG_NONE},
        .reserved = {},
        .args = {lhs, rhs, 0},
    };
  }

  static loom_predicate_t LiteralPredicate(loom_predicate_kind_t kind,
                                           loom_value_id_t value,
                                           int64_t literal) {
    return {
        .kind = kind,
        .arg_count = 2,
        .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST,
                     LOOM_PRED_ARG_NONE},
        .reserved = {},
        .args = {value, literal, 0},
    };
  }

  loom_value_id_t BuildConstant(int64_t value) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_index_constant_build(&builder_, loom_attr_i64(value),
                                            type_, LOOM_LOCATION_UNKNOWN, &op));
    IREE_CHECK_OK(loom_value_fact_table_compute_op(&table_, module_, op));
    return loom_index_constant_result(op);
  }

  loom_value_id_t DefineValue(loom_type_t type) {
    loom_value_id_t value = LOOM_VALUE_ID_INVALID;
    IREE_CHECK_OK(loom_builder_define_value(&builder_, type, &value));
    IREE_CHECK_OK(loom_value_fact_table_define(&table_, value,
                                               loom_value_facts_unknown()));
    return value;
  }

  iree_arena_block_pool_t pool_;
  iree_arena_allocator_t arena_;
  loom_context_t context_;
  loom_module_t* module_ = nullptr;
  loom_builder_t builder_;
  loom_value_fact_table_t table_;
  const loom_type_t type_ = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  loom_value_id_t inputs_[2];
};

TEST_F(FactRelationTest, RelationFreeIdentityAllocatesNoRelationState) {
  loom_op_t* identity = nullptr;
  IREE_ASSERT_OK(BuildIdentity(inputs_[0], &identity));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, identity));
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, IntegerOrderUsesIdentityAndExactFacts) {
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                nullptr, inputs_[0], inputs_[0]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, inputs_[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  IREE_ASSERT_OK(loom_value_fact_table_define(&table_, inputs_[0],
                                              loom_value_facts_exact_i64(4)));
  IREE_ASSERT_OK(loom_value_fact_table_define(&table_, inputs_[1],
                                              loom_value_facts_exact_i64(9)));
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, inputs_[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, inputs_[1], inputs_[0]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, IntegerOrderUsesIntervalsAndNonzeroFacts) {
  IREE_ASSERT_OK(loom_value_fact_table_define(
      &table_, inputs_[0], loom_value_facts_make(-16, -1, 1)));
  IREE_ASSERT_OK(loom_value_fact_table_define(&table_, inputs_[1],
                                              loom_value_facts_make(0, 16, 1)));
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, inputs_[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);

  loom_value_facts_t nonzero = loom_value_facts_unknown();
  nonzero.flags |= LOOM_VALUE_FACT_NON_ZERO;
  IREE_ASSERT_OK(loom_value_fact_table_define(&table_, inputs_[0], nonzero));
  IREE_ASSERT_OK(loom_value_fact_table_define(&table_, inputs_[1],
                                              loom_value_facts_exact_i64(0)));
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, inputs_[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
                LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, IntegerLiteralOrderUsesScalarFacts) {
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_literal_order(
                nullptr, inputs_[0], 42),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  IREE_ASSERT_OK(loom_value_fact_table_define(&table_, inputs_[0],
                                              loom_value_facts_make(4, 9, 1)));
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_literal_order(
                &table_, inputs_[0], 3),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_literal_order(
                &table_, inputs_[0], 7),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_literal_order(
                &table_, inputs_[0], 10),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, InteriorLiteralDisequalityRetainsScopedExclusion) {
  const loom_value_id_t literal = BuildConstant(42);
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0]}, {LiteralPredicate(LOOM_PREDICATE_NE, inputs_[0], 42)},
      &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];

  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                             literal),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
                LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_literal_order(
                &table_, result, 42),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
                LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  const loom_predicate_t repeated =
      LiteralPredicate(LOOM_PREDICATE_NE, result, 42);
  EXPECT_EQ(loom_value_fact_table_evaluate_predicate(&table_, &repeated),
            LOOM_DECISION_TRUTH_TRUE);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, inputs_[0], literal),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_literal_order(
                &table_, inputs_[0], 42),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
}

TEST_F(FactRelationTest, ValueAndLiteralAtomsUseIndependentNamespaces) {
  // inputs_[1] has value ID 1, deliberately matching the literal bits below.
  // Both relation atoms must remain independently queryable.
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(
      BuildAssume({inputs_[0]},
                  {RelationPredicate(LOOM_PREDICATE_GT, inputs_[0], inputs_[1]),
                   LiteralPredicate(LOOM_PREDICATE_NE, inputs_[0], 1)},
                  &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];
  const loom_value_id_t literal = BuildConstant(1);

  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                             inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                             literal),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
                LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
}

TEST_F(FactRelationTest, NonzeroLiteralUsesScalarFactsWithoutRelationState) {
  const loom_value_id_t zero = BuildConstant(0);
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0]}, {LiteralPredicate(LOOM_PREDICATE_NE, inputs_[0], 0)},
      &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];

  EXPECT_EQ(
      loom_value_fact_table_query_signed_integer_order(&table_, result, zero),
      LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
          LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, LiteralBoundsStayInScalarFacts) {
  const loom_value_id_t literal = BuildConstant(42);
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0]}, {LiteralPredicate(LOOM_PREDICATE_LT, inputs_[0], 42)},
      &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];

  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                             literal),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, LiteralExclusionsInternAndInheritAsPersistentSets) {
  std::vector<loom_value_id_t> literals;
  std::vector<loom_predicate_t> predicates;
  for (int64_t value = 1; value <= 10; ++value) {
    literals.push_back(BuildConstant(value));
    predicates.push_back(
        LiteralPredicate(LOOM_PREDICATE_NE, inputs_[0], value));
  }
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume({inputs_[0]}, predicates, &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  loom_value_id_t result = loom_index_assume_results(assume).values[0];

  for (int i = 0; i < 32; ++i) {
    loom_op_t* identity = nullptr;
    IREE_ASSERT_OK(BuildIdentity(result, &identity));
    IREE_ASSERT_OK(
        loom_value_fact_table_compute_op(&table_, module_, identity));
    result = loom_index_assume_results(identity).values[0];
  }
  for (loom_value_id_t literal : literals) {
    EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                               literal),
              LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
                  LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  }
}

TEST_F(FactRelationTest, SignedRelationsRetainEveryOrientation) {
  struct RelationCase {
    loom_predicate_kind_t predicate_kind;
    loom_symbolic_integer_relation_t relation;
  };
  const RelationCase cases[] = {
      {LOOM_PREDICATE_EQ, LOOM_SYMBOLIC_INTEGER_RELATION_EQ},
      {LOOM_PREDICATE_NE, LOOM_SYMBOLIC_INTEGER_RELATION_NE},
      {LOOM_PREDICATE_LT, LOOM_SYMBOLIC_INTEGER_RELATION_LT},
      {LOOM_PREDICATE_LE, LOOM_SYMBOLIC_INTEGER_RELATION_LE},
      {LOOM_PREDICATE_GT, LOOM_SYMBOLIC_INTEGER_RELATION_GT},
      {LOOM_PREDICATE_GE, LOOM_SYMBOLIC_INTEGER_RELATION_GE},
  };
  for (const RelationCase& relation_case : cases) {
    loom_op_t* assume = nullptr;
    IREE_ASSERT_OK(BuildAssume({inputs_[0], inputs_[1]},
                               {RelationPredicate(relation_case.predicate_kind,
                                                  inputs_[0], inputs_[1])},
                               &assume));
    IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
    const loom_value_slice_t results = loom_index_assume_results(assume);
    const loom_integer_order_outcome_bits_t expected =
        loom_symbolic_integer_relation_outcomes(relation_case.relation);
    EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                  &table_, results.values[0], results.values[1]),
              expected);
    EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                  &table_, results.values[1], results.values[0]),
              loom_integer_order_outcomes_swap(expected));
    EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                  &table_, results.values[0], inputs_[1]),
              expected);
    EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                  &table_, inputs_[0], results.values[1]),
              expected);
    EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                  &table_, inputs_[0], inputs_[1]),
              LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
    const loom_predicate_t repeated = RelationPredicate(
        relation_case.predicate_kind, results.values[0], results.values[1]);
    EXPECT_EQ(loom_value_fact_table_evaluate_predicate(&table_, &repeated),
              LOOM_DECISION_TRUTH_TRUE);
  }
}

TEST_F(FactRelationTest, UnsignedRelationDoesNotEnterSignedStore) {
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0], inputs_[1]},
      {RelationPredicate(LOOM_PREDICATE_ULT, inputs_[0], inputs_[1])},
      &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_slice_t results = loom_index_assume_results(assume);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], results.values[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  const loom_predicate_t repeated = RelationPredicate(
      LOOM_PREDICATE_ULT, results.values[0], results.values[1]);
  EXPECT_EQ(loom_value_fact_table_evaluate_predicate(&table_, &repeated),
            LOOM_DECISION_TRUTH_UNKNOWN);
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, FloatEqualityDoesNotEnterSignedStore) {
  const loom_type_t f32_type = loom_type_scalar(LOOM_SCALAR_TYPE_F32);
  const loom_value_id_t lhs = DefineValue(f32_type);
  const loom_value_id_t rhs = DefineValue(f32_type);
  const loom_value_id_t values[] = {lhs, rhs};
  const loom_type_t result_types[] = {f32_type, f32_type};
  loom_predicate_t predicate = RelationPredicate(LOOM_PREDICATE_EQ, lhs, rhs);
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(loom_scalar_assume_build(
      &builder_, values, IREE_ARRAYSIZE(values), &predicate, 1, result_types,
      IREE_ARRAYSIZE(result_types), LOOM_LOCATION_UNKNOWN, &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, DynamicRangeRetainsBothEndpoints) {
  const loom_value_id_t upper = DefineValue(type_);
  const loom_predicate_t range = {
      .kind = LOOM_PREDICATE_RANGE,
      .arg_count = 3,
      .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_VALUE,
                   LOOM_PRED_ARG_VALUE},
      .reserved = {},
      .args = {inputs_[0], inputs_[1], upper},
  };
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume({inputs_[0]}, {range}, &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                             inputs_[1]),
            loom_symbolic_integer_relation_outcomes(
                LOOM_SYMBOLIC_INTEGER_RELATION_GE));
  EXPECT_EQ(
      loom_value_fact_table_query_signed_integer_order(&table_, result, upper),
      loom_symbolic_integer_relation_outcomes(
          LOOM_SYMBOLIC_INTEGER_RELATION_LE));
  loom_predicate_t repeated = range;
  repeated.args[0] = result;
  EXPECT_EQ(loom_value_fact_table_evaluate_predicate(&table_, &repeated),
            LOOM_DECISION_TRUTH_TRUE);
}

TEST_F(FactRelationTest, TupleAliasesShareLocalRelationsButKeepInheritedScope) {
  const loom_value_id_t third = DefineValue(type_);
  loom_op_t* first = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0]},
      {RelationPredicate(LOOM_PREDICATE_LT, inputs_[0], inputs_[1])}, &first));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, first));
  const loom_value_id_t scoped = loom_index_assume_results(first).values[0];

  loom_op_t* tuple = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {scoped, inputs_[0], inputs_[1]},
      {RelationPredicate(LOOM_PREDICATE_GT, inputs_[0], third)}, &tuple));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, tuple));
  const loom_value_slice_t results = loom_index_assume_results(tuple);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[1], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], third),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[1], third),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);
}

TEST_F(FactRelationTest, RelationsPropagateAcrossDeepIdentityChains) {
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0]},
      {RelationPredicate(LOOM_PREDICATE_LE, inputs_[0], inputs_[1])}, &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  loom_value_id_t current = loom_index_assume_results(assume).values[0];
  for (int i = 0; i < 128; ++i) {
    loom_op_t* identity = nullptr;
    IREE_ASSERT_OK(BuildIdentity(current, &identity));
    IREE_ASSERT_OK(
        loom_value_fact_table_compute_op(&table_, module_, identity));
    current = loom_index_assume_results(identity).values[0];
  }
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, current,
                                                             inputs_[1]),
            loom_symbolic_integer_relation_outcomes(
                LOOM_SYMBOLIC_INTEGER_RELATION_LE));
}

TEST_F(FactRelationTest, ContradictoryRelationsProveNoOutcome) {
  loom_op_t* first = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0]},
      {RelationPredicate(LOOM_PREDICATE_LT, inputs_[0], inputs_[1])}, &first));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, first));
  const loom_value_id_t first_result =
      loom_index_assume_results(first).values[0];
  loom_op_t* second = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {first_result},
      {RelationPredicate(LOOM_PREDICATE_GT, inputs_[0], inputs_[1])}, &second));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, second));
  EXPECT_EQ(
      loom_value_fact_table_query_signed_integer_order(
          &table_, loom_index_assume_results(second).values[0], inputs_[1]),
      LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
}

TEST_F(FactRelationTest, RelationMutationUndefinitionAndScopeReuse) {
  loom_predicate_t predicate =
      RelationPredicate(LOOM_PREDICATE_LT, inputs_[0], inputs_[1]);
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume({inputs_[0], inputs_[1]}, {predicate}, &assume));
  bool changed = false;
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_FALSE(changed);
  const loom_value_slice_t results = loom_index_assume_results(assume);

  predicate.kind = LOOM_PREDICATE_GT;
  IREE_ASSERT_OK(loom_index_assume_set_predicates(
      module_, assume, loom_attr_predicate_list(&predicate, 1)));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], results.values[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);

  IREE_ASSERT_OK(loom_index_assume_set_predicates(
      module_, assume, loom_attr_predicate_list(nullptr, 0)));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], results.values[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);

  predicate.kind = LOOM_PREDICATE_LT;
  IREE_ASSERT_OK(loom_index_assume_set_predicates(
      module_, assume, loom_attr_predicate_list(&predicate, 1)));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  loom_value_fact_table_undefine(&table_, results.values[0]);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);

  loom_value_fact_table_clear_scope(&table_);
  EXPECT_EQ(table_.integer_relations, nullptr);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                &table_, results.values[0], inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
}

TEST_F(FactRelationTest, LiteralRelationLifecycleAndTransfer) {
  const loom_value_id_t literal = BuildConstant(42);
  loom_predicate_t predicate =
      LiteralPredicate(LOOM_PREDICATE_NE, inputs_[0], 42);
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume({inputs_[0]}, {predicate}, &assume));
  bool changed = false;
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_FALSE(changed);
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];

  IREE_ASSERT_OK(loom_index_assume_set_predicates(
      module_, assume, loom_attr_predicate_list(nullptr, 0)));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                             literal),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);

  IREE_ASSERT_OK(loom_index_assume_set_predicates(
      module_, assume, loom_attr_predicate_list(&predicate, 1)));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  loom_value_fact_table_undefine(&table_, result);
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&table_, result,
                                                             literal),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL);
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));

  const loom_value_id_t selected[] = {result, literal};
  loom_value_fact_table_t clone = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&clone, &arena_, 0));
  IREE_ASSERT_OK(loom_value_fact_table_clone_values(
      &clone, {&table_, selected, IREE_ARRAYSIZE(selected)}, module_));
  EXPECT_EQ(
      loom_value_fact_table_query_signed_integer_order(&clone, result, literal),
      LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
          LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);

  loom_value_fact_table_t seed = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&seed, &arena_, 0));
  IREE_ASSERT_OK(loom_value_fact_table_seed_values(
      &seed, {&table_, selected, IREE_ARRAYSIZE(selected)}, module_));
  EXPECT_EQ(
      loom_value_fact_table_query_signed_integer_order(&seed, result, literal),
      LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS |
          LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER);

  loom_value_fact_table_clear_scope(&table_);
  EXPECT_EQ(table_.integer_relations, nullptr);
}

TEST_F(FactRelationTest, CloneAndSeedPreserveSelectedRelationScope) {
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume(
      {inputs_[0]},
      {RelationPredicate(LOOM_PREDICATE_LT, inputs_[0], inputs_[1])}, &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];

  loom_value_fact_table_t clone = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&clone, &arena_, 0));
  IREE_ASSERT_OK(loom_value_fact_table_clone_values(
      &clone, {&table_, &result, 1}, module_));
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&clone, result,
                                                             inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);

  loom_value_fact_table_t seed = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&seed, &arena_, 0));
  IREE_ASSERT_OK(
      loom_value_fact_table_seed_values(&seed, {&table_, &result, 1}, module_));
  EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(&seed, result,
                                                             inputs_[1]),
            LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS);
}

TEST_F(FactRelationTest, LargeTupleUsesGroupedRelationPlan) {
  constexpr uint16_t kValueCount = 24;
  std::vector<loom_value_id_t> values;
  values.reserve(kValueCount);
  for (uint16_t i = 0; i < kValueCount; ++i) {
    values.push_back(DefineValue(type_));
  }
  std::vector<loom_predicate_t> predicates;
  for (uint16_t i = 0; i < 5; ++i) {
    predicates.push_back(
        RelationPredicate(LOOM_PREDICATE_LE, values[i], values.back()));
  }
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(BuildAssume(values, predicates, &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_slice_t results = loom_index_assume_results(assume);
  for (uint16_t i = 0; i < predicates.size(); ++i) {
    EXPECT_EQ(loom_value_fact_table_query_signed_integer_order(
                  &table_, results.values[i], results.values[kValueCount - 1]),
              loom_symbolic_integer_relation_outcomes(
                  LOOM_SYMBOLIC_INTEGER_RELATION_LE));
  }
}

}  // namespace
}  // namespace loom
