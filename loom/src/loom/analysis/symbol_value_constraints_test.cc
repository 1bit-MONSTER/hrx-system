// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/symbol_value_constraints.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/attribute.h"

namespace {

TEST(SymbolValueConstraintsTest, ChecksExactIntegerValue) {
  const loom_value_id_t contract_value = 7;
  loom_predicate_t predicates[] = {
      {
          /*.kind=*/LOOM_PREDICATE_GE,
          /*.arg_count=*/2,
          /*.arg_tags=*/{LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
          /*.reserved=*/{},
          /*.args=*/{contract_value, 32},
      },
      {
          /*.kind=*/LOOM_PREDICATE_MUL,
          /*.arg_count=*/2,
          /*.arg_tags=*/{LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
          /*.reserved=*/{},
          /*.args=*/{contract_value, 16},
      },
      {
          /*.kind=*/LOOM_PREDICATE_POW2,
          /*.arg_count=*/1,
          /*.arg_tags=*/{LOOM_PRED_ARG_VALUE},
          /*.reserved=*/{},
          /*.args=*/{contract_value},
      },
  };

  IREE_ASSERT_OK(loom_symbol_value_constraints_check_exact(
      IREE_SV("tile_size"), loom_type_scalar(LOOM_SCALAR_TYPE_I32),
      contract_value, loom_attr_i64(64),
      loom_attr_predicate_list(predicates, IREE_ARRAYSIZE(predicates))));
}

TEST(SymbolValueConstraintsTest, RejectsViolatedIntegerPredicate) {
  const loom_value_id_t contract_value = 3;
  loom_predicate_t predicate = {
      /*.kind=*/LOOM_PREDICATE_RANGE,
      /*.arg_count=*/3,
      /*.arg_tags=*/
      {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST, LOOM_PRED_ARG_CONST},
      /*.reserved=*/{},
      /*.args=*/{contract_value, 16, 63},
  };

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_symbol_value_constraints_check_exact(
          IREE_SV("tile_size"), loom_type_scalar(LOOM_SCALAR_TYPE_I32),
          contract_value, loom_attr_i64(64),
          loom_attr_predicate_list(&predicate, 1)));
}

TEST(SymbolValueConstraintsTest, ChecksUnsignedCarrierOrder) {
  const loom_value_id_t contract_value = 3;
  loom_predicate_t predicate = {
      /*.kind=*/LOOM_PREDICATE_ULE,
      /*.arg_count=*/2,
      /*.arg_tags=*/{LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
      /*.reserved=*/{},
      /*.args=*/{contract_value, -1},
  };

  IREE_ASSERT_OK(loom_symbol_value_constraints_check_exact(
      IREE_SV("tile_size"), loom_type_scalar(LOOM_SCALAR_TYPE_I32),
      contract_value, loom_attr_i64(INT32_MIN),
      loom_attr_predicate_list(&predicate, 1)));

  predicate.args[1] = INT32_MIN;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_symbol_value_constraints_check_exact(
          IREE_SV("tile_size"), loom_type_scalar(LOOM_SCALAR_TYPE_I32),
          contract_value, loom_attr_i64(-1),
          loom_attr_predicate_list(&predicate, 1)));
}

TEST(SymbolValueConstraintsTest, RejectsNonPositiveMultipleDivisor) {
  const loom_value_id_t contract_value = 3;
  for (int64_t divisor : {INT64_C(-16), INT64_C(0)}) {
    loom_predicate_t predicate = {
        /*.kind=*/LOOM_PREDICATE_MUL,
        /*.arg_count=*/2,
        /*.arg_tags=*/{LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
        /*.reserved=*/{},
        /*.args=*/{contract_value, divisor},
    };

    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_symbol_value_constraints_check_exact(
            IREE_SV("tile_size"), loom_type_scalar(LOOM_SCALAR_TYPE_I32),
            contract_value, loom_attr_i64(64),
            loom_attr_predicate_list(&predicate, 1)));
  }
}

TEST(SymbolValueConstraintsTest, IgnoresPredicatesForOtherValues) {
  const loom_value_id_t contract_value = 3;
  loom_predicate_t predicate = {
      /*.kind=*/LOOM_PREDICATE_EQ,
      /*.arg_count=*/2,
      /*.arg_tags=*/{LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
      /*.reserved=*/{},
      /*.args=*/{contract_value + 1, 0},
  };

  IREE_ASSERT_OK(loom_symbol_value_constraints_check_exact(
      IREE_SV("tile_size"), loom_type_scalar(LOOM_SCALAR_TYPE_I32),
      contract_value, loom_attr_i64(64),
      loom_attr_predicate_list(&predicate, 1)));
}

}  // namespace
