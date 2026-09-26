// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/placement.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/codegen/low/builder.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/target/registers.h"

namespace loom {
namespace {

TEST(LowPlacementTest, ClassifiesEdgeCauses) {
  EXPECT_FALSE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_UNKNOWN));
  EXPECT_FALSE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT));
  EXPECT_FALSE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_LOW_COPY));
  EXPECT_FALSE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_LOW_MOVE));
  EXPECT_FALSE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_LOW_SLICE));
  EXPECT_FALSE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT));
  EXPECT_TRUE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_LOW_BRANCH));
  EXPECT_TRUE(loom_low_placement_cause_is_edge(
      LOOM_LOW_PLACEMENT_CAUSE_LOW_SCF_LOOP_ENTRY));
  EXPECT_TRUE(
      loom_low_placement_cause_is_edge(LOOM_LOW_PLACEMENT_CAUSE_LOW_SCF_YIELD));
  EXPECT_TRUE(loom_low_placement_cause_is_edge(
      LOOM_LOW_PLACEMENT_CAUSE_LOW_SCF_CONDITION));
  EXPECT_FALSE(loom_low_placement_cause_is_edge(
      LOOM_LOW_PLACEMENT_CAUSE_SCHEDULE_PAIR_AFFINITY));
}

TEST(LowPlacementTest, ComposedRelationHasNoDirectSourceOperand) {
  loom_low_placement_relation_t source_to_intermediate = {};
  source_to_intermediate.result_ordinal = 1;
  source_to_intermediate.source_ordinal = 0;
  source_to_intermediate.result_unit_offset = 2;
  source_to_intermediate.source_unit_offset = 4;
  source_to_intermediate.unit_count = 3;
  source_to_intermediate.source_operand_index = 1;
  loom_low_placement_relation_t intermediate_to_result = {};
  intermediate_to_result.result_ordinal = 2;
  intermediate_to_result.source_ordinal = 1;
  intermediate_to_result.result_unit_offset = 8;
  intermediate_to_result.source_unit_offset = 3;
  intermediate_to_result.unit_count = 3;
  intermediate_to_result.source_operand_index = 2;

  loom_low_placement_relation_t composed = {};
  ASSERT_TRUE(loom_low_placement_relation_compose(
      &source_to_intermediate, &intermediate_to_result, &composed));
  EXPECT_EQ(composed.source_ordinal, 0u);
  EXPECT_EQ(composed.result_ordinal, 2u);
  EXPECT_EQ(composed.source_unit_offset, 5u);
  EXPECT_EQ(composed.result_unit_offset, 8u);
  EXPECT_EQ(composed.unit_count, 2u);
  EXPECT_EQ(composed.source_operand_index,
            LOOM_LOW_PLACEMENT_SOURCE_OPERAND_NONE);
}

TEST(LowPlacementTest, RetainsOperandAlignmentAcrossExactTiesOnly) {
  iree_arena_block_pool_t pool;
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool);
  loom_context_t context;
  loom_context_initialize(iree_allocator_system(), &context);
  iree_host_size_t vtable_count = 0;
  const auto* vtables = loom_low_dialect_vtables(&vtable_count);
  IREE_ASSERT_OK(loom_context_register_dialect(
      &context, LOOM_DIALECT_LOW, vtables, (uint16_t)vtable_count));
  IREE_ASSERT_OK(loom_context_finalize(&context));

  // The two classes are alternatives of the same instruction field, with
  // different hardware alignment. An earlier tied chain has no requirement
  // of its own; its final consumer determines the whole component's base.
  loom_low_reg_class_t classes[2] = {};
  for (auto& reg_class : classes) {
    reg_class.alloc_unit_bits = 32;
  }
  const loom_low_reg_class_alt_t alternatives[] = {
      {0, LOOM_LOW_REG_CLASS_ALT_FLAG_PREFERRED, 0},
      {1, 0, 0},
      {0, LOOM_LOW_REG_CLASS_ALT_FLAG_PREFERRED, 3},
      {1, 0, 1},
  };
  loom_low_operand_t operands[3] = {};
  for (auto& operand : operands) {
    operand.role = LOOM_LOW_OPERAND_ROLE_OPERAND;
    operand.reg_class_alt_count = 2;
    operand.unit_count = 1;
  }
  operands[0].role = LOOM_LOW_OPERAND_ROLE_RESULT;
  operands[2].reg_class_alt_start = 2;
  const loom_low_constraint_t tie_constraint = {LOOM_LOW_CONSTRAINT_KIND_TIED,
                                                0, 1, 0};
  loom_low_descriptor_t descriptors[2] = {};
  descriptors[0].operand_count = 2;
  descriptors[0].result_count = 1;
  descriptors[0].minimum_packet_operand_count = 1;
  descriptors[0].constraint_count = 1;
  descriptors[1].operand_start = 2;
  descriptors[1].operand_count = 1;
  descriptors[1].minimum_packet_operand_count = 1;
  loom_low_descriptor_set_t descriptor_set = {};
  descriptor_set.stable_id = 1;
  descriptor_set.reg_classes = classes;
  descriptor_set.reg_class_count = IREE_ARRAYSIZE(classes);
  descriptor_set.reg_class_alts = alternatives;
  descriptor_set.reg_class_alt_count = IREE_ARRAYSIZE(alternatives);
  descriptor_set.operands = operands;
  descriptor_set.operand_count = IREE_ARRAYSIZE(operands);
  descriptor_set.descriptors = descriptors;
  descriptor_set.descriptor_count = IREE_ARRAYSIZE(descriptors);
  descriptor_set.constraints = &tie_constraint;
  descriptor_set.constraint_count = 1;

  for (uint16_t class_id : {0, 1}) {
    loom_module_t* module = nullptr;
    IREE_ASSERT_OK(loom_module_allocate(&context, IREE_SV("alignment"), &pool,
                                        nullptr, iree_allocator_system(),
                                        &module));
    loom_builder_t builder;
    loom_builder_initialize(module, &module->arena, loom_module_block(module),
                            &builder);
    loom_string_id_t name;
    IREE_ASSERT_OK(
        loom_builder_intern_string(&builder, IREE_SV("alignment"), &name));
    loom_symbol_id_t symbol;
    IREE_ASSERT_OK(loom_module_add_symbol(module, name, &symbol));
    const loom_type_t type = loom_low_register_type(1, class_id, 1);
    loom_op_t* function = nullptr;
    IREE_ASSERT_OK(loom_low_func_def_build(
        &builder, 0, 0, 0, 0, 0, 0, 0, 0, name, {}, 0, {}, {},
        LOOM_STRING_ID_INVALID, {}, loom_symbol_ref_t{0, symbol}, &type, 1,
        nullptr, 0, nullptr, 0, nullptr, 0, LOOM_LOCATION_UNKNOWN, &function));
    loom_region_t* body = loom_low_func_def_body(function);
    loom_builder_enter_region(&builder, function, body);
    const loom_value_id_t source = loom_region_entry_arg_id(body, 0);
    loom_op_t* copy = nullptr;
    IREE_ASSERT_OK(loom_low_copy_build(&builder, source, false, type,
                                       LOOM_LOCATION_UNKNOWN, &copy));
    loom_value_id_t chain[] = {loom_low_copy_result(copy), 0, 0};
    const loom_tied_result_t tie = {0, 0, false};
    for (unsigned i = 1; i < IREE_ARRAYSIZE(chain); ++i) {
      loom_op_t* op = nullptr;
      IREE_ASSERT_OK(loom_low_build_resolved_descriptor_op(
          &builder, &descriptor_set, &descriptors[0], 0, &chain[i - 1], 1, {},
          &type, 1, &tie, 1, LOOM_LOCATION_UNKNOWN, &op));
      chain[i] = loom_op_results(op)[0];
    }
    loom_op_t* consumer = nullptr;
    IREE_ASSERT_OK(loom_low_build_resolved_descriptor_op(
        &builder, &descriptor_set, &descriptors[1], 0, &chain[2], 1, {},
        nullptr, 0, nullptr, 0, LOOM_LOCATION_UNKNOWN, &consumer));
    loom_op_t* terminator = nullptr;
    IREE_ASSERT_OK(loom_low_return_build(&builder, nullptr, 0,
                                         LOOM_LOCATION_UNKNOWN, &terminator));
    loom_local_value_domain_t domain = {};
    IREE_ASSERT_OK(loom_local_value_domain_acquire_for_region(
        module, body, &module->arena, &domain));
    loom_liveness_analysis_t liveness = {};
    IREE_ASSERT_OK(loom_liveness_analyze_local_value_domain(
        &domain, loom_liveness_order_empty(), &module->arena, &liveness));
    loom_low_placement_table_t placement = {};
    IREE_ASSERT_OK(loom_low_placement_analyze_region(
        module, body, &descriptor_set, &domain, &liveness, {}, &module->arena,
        &placement));
    ASSERT_NE(placement.unit_alignment_log2_by_interval, nullptr);
    for (auto value : chain) {
      const auto ordinal = loom_local_value_domain_try_ordinal(&domain, value);
      EXPECT_EQ(placement.unit_alignment_log2_by_interval
                    [liveness.value_interval_indices[ordinal]],
                class_id == 0 ? 3 : 1);
    }
    const auto source_ordinal =
        loom_local_value_domain_try_ordinal(&domain, source);
    EXPECT_EQ(placement.unit_alignment_log2_by_interval
                  [liveness.value_interval_indices[source_ordinal]],
              0);
    loom_local_value_domain_release(&domain);
    loom_module_free(module);
  }
  loom_context_deinitialize(&context);
  iree_arena_block_pool_deinitialize(&pool);
}

}  // namespace
}  // namespace loom
