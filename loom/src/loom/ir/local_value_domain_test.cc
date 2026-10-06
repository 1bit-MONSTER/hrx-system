// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/local_value_domain.h"

#include <set>
#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ops/test/ops.h"

namespace loom {
namespace {

class LocalValueDomainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = loom_test_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_TEST, vtables, static_cast<uint16_t>(count)));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("domain"), &pool_,
                                        nullptr, iree_allocator_system(),
                                        &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
  }

  void TearDown() override {
    loom_local_value_domain_release(&domain_);
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  loom_value_id_t Constant(int64_t value, loom_scalar_type_t type) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_constant_build(&builder_, loom_attr_i64(value),
                                           loom_type_scalar(type),
                                           LOOM_LOCATION_UNKNOWN, &op));
    return loom_test_constant_result(op);
  }

  loom_op_t* Region(loom_value_id_t condition) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_optional_region_build(&builder_, 0, condition,
                                                  LOOM_LOCATION_UNKNOWN, &op));
    return op;
  }

  void Yield(loom_value_id_t value) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_yield_build(&builder_, &value, 1,
                                        LOOM_LOCATION_UNKNOWN, &op));
  }

  void ExpectPartition(std::initializer_list<loom_value_id_t> definitions,
                       std::initializer_list<loom_value_id_t> captures) {
    EXPECT_EQ(domain_.definition_count, definitions.size());
    EXPECT_EQ(domain_.value_count, definitions.size() + captures.size());
    EXPECT_EQ(
        std::set<loom_value_id_t>(domain_.value_ids,
                                  domain_.value_ids + domain_.definition_count),
        std::set<loom_value_id_t>(definitions));
    EXPECT_EQ(
        std::set<loom_value_id_t>(domain_.value_ids + domain_.definition_count,
                                  domain_.value_ids + domain_.value_count),
        std::set<loom_value_id_t>(captures));
    for (loom_value_ordinal_t i = 0; i < domain_.value_count; ++i) {
      EXPECT_EQ(loom_local_value_domain_ordinal(&domain_, domain_.value_ids[i]),
                i);
    }
  }

  // Backing storage shared by the module and acquired domains.
  iree_arena_block_pool_t pool_ = {};
  // Lifetime of the local domain's compact value array.
  iree_arena_allocator_t arena_ = {};
  // Test dialect registered for the production builder APIs.
  loom_context_t context_ = {};
  // Module containing source definitions and captures.
  loom_module_t* module_ = nullptr;
  // Builder positioned in the current fixture region.
  loom_builder_t builder_ = {};
  // Acquired domain, released even when an assertion ends a test.
  loom_local_value_domain_t domain_ = {};
};

TEST_F(LocalValueDomainTest, RetainsNestedDefinitionsAndCapturedTypeProviders) {
  const auto condition = Constant(1, LOOM_SCALAR_TYPE_I1);
  const auto width = Constant(8, LOOM_SCALAR_TYPE_INDEX);
  loom_op_t* outer = Region(condition);
  loom_region_t* body = loom_test_optional_region_body(outer);
  loom_builder_enter_region(&builder_, outer, body);
  const auto outer_value = Constant(4, LOOM_SCALAR_TYPE_INDEX);
  loom_op_t* inner = Region(condition);
  loom_region_t* inner_body = loom_test_optional_region_body(inner);
  loom_builder_ip_t previous =
      loom_builder_enter_region(&builder_, inner, inner_body);
  const auto inner_value = Constant(2, LOOM_SCALAR_TYPE_INDEX);
  const loom_type_t tensor_type = loom_type_shaped_1d(
      LOOM_TYPE_TENSOR, LOOM_SCALAR_TYPE_I32, loom_dim_pack_dynamic(width), 0);
  loom_value_id_t argument = LOOM_VALUE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_define_value(module_, tensor_type, &argument));
  IREE_ASSERT_OK(loom_block_add_arg(
      module_, loom_region_entry_block(inner_body), argument));
  Yield(inner_value);
  loom_builder_restore(&builder_, previous);
  Yield(outer_value);

  IREE_ASSERT_OK(loom_local_value_domain_acquire_for_region(module_, body,
                                                            &arena_, &domain_));
  ExpectPartition({outer_value}, {condition, width});
  EXPECT_EQ(loom_local_value_domain_try_ordinal(&domain_, inner_value),
            LOOM_VALUE_ORDINAL_INVALID);
  loom_local_value_domain_release(&domain_);

  IREE_ASSERT_OK(loom_local_value_domain_acquire_for_region_tree(
      module_, body, &arena_, &domain_));
  ExpectPartition({outer_value, inner_value, argument}, {condition, width});
  const std::vector<loom_value_id_t> ids(
      domain_.value_ids, domain_.value_ids + domain_.value_count);
  loom_local_value_domain_release(&domain_);
  for (auto id : ids) {
    EXPECT_EQ(loom_value_u32_scratch_load(&module_->scratch.values, id),
              LOOM_VALUE_ORDINAL_INVALID);
  }
}

TEST_F(LocalValueDomainTest,
       PromotesForwardDefinitionsBeforePublishingOrdinals) {
  const auto condition = Constant(1, LOOM_SCALAR_TYPE_I1);
  const auto capture = Constant(3, LOOM_SCALAR_TYPE_INDEX);
  loom_op_t* outer = Region(condition);
  loom_region_t* body = loom_test_optional_region_body(outer);
  loom_block_t* use_block = nullptr;
  loom_block_t* definition_block = nullptr;
  IREE_ASSERT_OK(loom_region_append_block(module_, body, &use_block));
  IREE_ASSERT_OK(loom_region_append_block(module_, body, &definition_block));
  loom_builder_enter_region(&builder_, outer, body);
  loom_op_t* branch = nullptr;
  IREE_ASSERT_OK(loom_test_br_build(&builder_, definition_block,
                                    LOOM_LOCATION_UNKNOWN, &branch));
  loom_builder_initialize(module_, &module_->arena, definition_block,
                          &builder_);
  const auto definition = Constant(7, LOOM_SCALAR_TYPE_INDEX);
  IREE_ASSERT_OK(
      loom_test_br_build(&builder_, use_block, LOOM_LOCATION_UNKNOWN, &branch));
  loom_builder_initialize(module_, &module_->arena, use_block, &builder_);
  loom_op_t* sum = nullptr;
  IREE_ASSERT_OK(loom_test_addi_build(&builder_, definition, capture,
                                      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
                                      LOOM_LOCATION_UNKNOWN, &sum));
  Yield(loom_test_addi_result(sum));

  IREE_ASSERT_OK(loom_local_value_domain_acquire_for_region_tree(
      module_, body, &arena_, &domain_));
  ExpectPartition({definition, loom_test_addi_result(sum)}, {capture});
  const std::vector<loom_value_id_t> ids(
      domain_.value_ids, domain_.value_ids + domain_.value_count);
  loom_value_id_t new_value = LOOM_VALUE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_define_value(
      module_, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), &new_value));
  loom_value_ordinal_t ordinal = LOOM_VALUE_ORDINAL_INVALID;
  IREE_ASSERT_OK(loom_local_value_domain_register_value(&domain_, &arena_,
                                                        new_value, &ordinal));
  EXPECT_EQ(ordinal, ids.size());
  EXPECT_EQ(domain_.definition_count, 2u);
  for (loom_value_ordinal_t i = 0; i < ids.size(); ++i) {
    EXPECT_EQ(loom_local_value_domain_ordinal(&domain_, ids[i]), i);
  }
  IREE_ASSERT_OK(loom_local_value_domain_register_value(&domain_, &arena_,
                                                        definition, &ordinal));
  EXPECT_EQ(domain_.value_ids[ordinal], definition);
  EXPECT_EQ(domain_.value_count, ids.size() + 1);
}

}  // namespace
}  // namespace loom
