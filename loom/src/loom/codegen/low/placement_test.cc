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

TEST(LowPlacementTest, DefiningTransferPrecedesEarlierCollectedUses) {
  iree_arena_block_pool_t pool;
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool);
  loom_context_t context;
  loom_context_initialize(iree_allocator_system(), &context);
  iree_host_size_t vtable_count = 0;
  const auto* vtables = loom_low_dialect_vtables(&vtable_count);
  IREE_ASSERT_OK(loom_context_register_dialect(
      &context, LOOM_DIALECT_LOW, vtables, (uint16_t)vtable_count));
  IREE_ASSERT_OK(loom_context_finalize(&context));

  loom_low_reg_class_t classes[2] = {};
  loom_low_physical_register_t registers[2] = {};
  const uint16_t candidates[] = {0, 1};
  const uint16_t candidate_ordinals[] = {0, 0};
  const uint16_t atomic_units[] = {0, 1};
  const loom_low_reg_class_alt_t alternatives[] = {
      {0, LOOM_LOW_REGISTER_PART_NONE, LOOM_LOW_REG_CLASS_ALT_FLAG_PREFERRED,
       0},
      {1, LOOM_LOW_REGISTER_PART_NONE, LOOM_LOW_REG_CLASS_ALT_FLAG_PREFERRED,
       0},
  };
  loom_low_operand_t operands[2] = {};
  for (uint16_t i = 0; i < 2; ++i) {
    classes[i].target_bank_id = i;
    classes[i].flags = LOOM_LOW_REG_CLASS_FLAG_PHYSICAL |
                       LOOM_LOW_REG_CLASS_FLAG_EXPLICIT_PHYSICAL_REGISTERS;
    classes[i].alloc_unit_bits = 32;
    classes[i].allocatable_count = 1;
    classes[i].physical_register_candidate_start = i;
    classes[i].candidate_lookup = {i, i, 1};
    classes[i].physical_atomic_unit_count = 1;
    registers[i].atomic_unit_start = i;
    registers[i].atomic_unit_count = 1;
    operands[i].role = LOOM_LOW_OPERAND_ROLE_OPERAND;
    operands[i].source_value_index = i;
    operands[i].reg_class_alt_start = i;
    operands[i].reg_class_alt_count = 1;
    operands[i].unit_count = 1;
  }
  const loom_low_constraint_t constraint = {
      LOOM_LOW_CONSTRAINT_KIND_SAME_REGISTER_ORDINAL, 0, 1, 0};
  loom_low_descriptor_t descriptor = {};
  descriptor.operand_count = 2;
  descriptor.minimum_packet_operand_count = 2;
  descriptor.constraint_count = 1;
  loom_low_descriptor_set_t descriptor_set = {};
  descriptor_set.stable_id = 1;
  descriptor_set.reg_classes = classes;
  descriptor_set.reg_class_count = IREE_ARRAYSIZE(classes);
  descriptor_set.physical_registers = registers;
  descriptor_set.physical_register_count = IREE_ARRAYSIZE(registers);
  descriptor_set.physical_register_candidate_ids = candidates;
  descriptor_set.physical_register_candidate_count = IREE_ARRAYSIZE(candidates);
  descriptor_set.physical_register_candidate_ordinals = candidate_ordinals;
  descriptor_set.physical_register_atomic_units = atomic_units;
  descriptor_set.physical_register_atomic_unit_count =
      IREE_ARRAYSIZE(atomic_units);
  descriptor_set.reg_class_alts = alternatives;
  descriptor_set.reg_class_alt_count = IREE_ARRAYSIZE(alternatives);
  descriptor_set.operands = operands;
  descriptor_set.operand_count = IREE_ARRAYSIZE(operands);
  descriptor_set.descriptors = &descriptor;
  descriptor_set.descriptor_count = 1;
  descriptor_set.constraints = &constraint;
  descriptor_set.constraint_count = 1;

  const loom_low_placement_value_ref_t values[] = {
      {0, LOOM_LOW_PLACEMENT_VALUE_OPERAND, 0},
      {0, LOOM_LOW_PLACEMENT_VALUE_OPERAND, 1}};
  const loom_low_placement_predicate_t predicate = {
      0, 1, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 1};
  const loom_low_placement_clause_t clause = {0, 1, 1,
                                              LOOM_LOW_PLACEMENT_CLAUSE_ANY};
  const loom_low_placement_preference_t preference = {values, &predicate,
                                                      &clause, 2, 1};
  const uint16_t preference_indices[] = {1};

  for (bool is_move : {false, true}) {
    loom_module_t* module = nullptr;
    IREE_ASSERT_OK(loom_module_allocate(&context, IREE_SV("transfer"), &pool,
                                        nullptr, iree_allocator_system(),
                                        &module));
    loom_builder_t builder;
    loom_builder_initialize(module, &module->arena, loom_module_block(module),
                            &builder);
    loom_string_id_t name;
    IREE_ASSERT_OK(
        loom_builder_intern_string(&builder, IREE_SV("transfer"), &name));
    loom_symbol_id_t symbol;
    IREE_ASSERT_OK(loom_module_add_symbol(module, name, &symbol));
    const loom_type_t types[] = {loom_low_register_type(1, 0, 1),
                                 loom_low_register_type(1, 1, 1)};
    loom_op_t* function = nullptr;
    IREE_ASSERT_OK(loom_low_func_def_build(
        &builder, 0, 0, 0, 0, 0, 0, 0, 0, name, {}, 0, {}, {},
        LOOM_STRING_ID_INVALID, {}, loom_symbol_ref_t{0, symbol}, types, 2,
        nullptr, 0, nullptr, 0, nullptr, 0, LOOM_LOCATION_UNKNOWN, &function));
    loom_region_t* body = loom_low_func_def_body(function);
    loom_builder_enter_region(&builder, function, body);
    const loom_value_id_t source = loom_region_entry_arg_id(body, 0);
    const loom_value_id_t peer = loom_region_entry_arg_id(body, 1);

    // Execution is entry -> producer -> consumer. Region layout visits the
    // consumer first, so its input constraint precedes the defining transfer
    // in the collector. Both branch payloads exercise the reverse edge index.
    loom_block_t* consumer = nullptr;
    loom_block_t* producer = nullptr;
    IREE_ASSERT_OK(loom_region_append_block(module, body, &consumer));
    IREE_ASSERT_OK(loom_region_append_block(module, body, &producer));
    loom_value_id_t forwarded = LOOM_VALUE_ID_INVALID;
    IREE_ASSERT_OK(loom_module_define_value(module, types[1], &forwarded));
    IREE_ASSERT_OK(loom_block_add_arg(module, producer, forwarded));
    loom_value_id_t received = LOOM_VALUE_ID_INVALID;
    IREE_ASSERT_OK(loom_module_define_value(module, types[1], &received));
    IREE_ASSERT_OK(loom_block_add_arg(module, consumer, received));
    loom_op_t* entry_branch = nullptr;
    IREE_ASSERT_OK(loom_low_br_build(&builder, producer, &peer, 1,
                                     LOOM_LOCATION_UNKNOWN, &entry_branch));
    loom_builder_set_block(&builder, producer);
    loom_op_t* transfer = nullptr;
    IREE_ASSERT_OK(is_move
                       ? loom_low_move_build(&builder, source, false, types[0],
                                             LOOM_LOCATION_UNKNOWN, &transfer)
                       : loom_low_copy_build(&builder, source, false, types[0],
                                             LOOM_LOCATION_UNKNOWN, &transfer));
    const loom_value_id_t result = loom_op_results(transfer)[0];
    loom_op_t* producer_branch = nullptr;
    IREE_ASSERT_OK(loom_low_br_build(&builder, consumer, &forwarded, 1,
                                     LOOM_LOCATION_UNKNOWN, &producer_branch));
    loom_builder_set_block(&builder, consumer);
    const loom_value_id_t inputs[] = {result, received};
    loom_op_t* use = nullptr;
    IREE_ASSERT_OK(loom_low_build_resolved_descriptor_op(
        &builder, &descriptor_set, &descriptor, 0, inputs, 2, {}, nullptr, 0,
        nullptr, 0, LOOM_LOCATION_UNKNOWN, &use));
    loom_op_t* terminator = nullptr;
    IREE_ASSERT_OK(loom_low_return_build(&builder, nullptr, 0,
                                         LOOM_LOCATION_UNKNOWN, &terminator));

    loom_local_value_domain_t domain = {};
    IREE_ASSERT_OK(loom_local_value_domain_acquire_for_region(
        module, body, &module->arena, &domain));
    loom_liveness_analysis_t liveness = {};
    IREE_ASSERT_OK(loom_liveness_analyze_local_value_domain(
        &domain, loom_liveness_order_empty(), &module->arena, &liveness));
    for (iree_host_size_t i = 0; i < liveness.operation_count; ++i) {
      if (liveness.operation_points[i].op == use) {
        break;
      }
      ASSERT_NE(liveness.operation_points[i].op, transfer);
    }
    loom_low_placement_table_t placement = {};
    loom_low_placement_preference_index_t preferences = {};
    IREE_ASSERT_OK(loom_low_placement_analyze_region(
        module, body, &descriptor_set, &domain, &liveness, {},
        {preference_indices, &preference}, &module->arena, &module->arena,
        &placement, &preferences));
    // Explicit physical IDs are not linear bank coordinates. This target
    // preference is inapplicable even though both inputs are registers.
    EXPECT_EQ(preferences.use_count, 0u);
    EXPECT_EQ(preferences.offsets_by_origin, nullptr);
    const auto ordinal = loom_local_value_domain_try_ordinal(&domain, result);
    const auto range = placement.ranges_by_result_ordinal[ordinal];
    ASSERT_EQ(range.count, 2u);
    const auto* defining =
        loom_low_placement_defining_transfer_for_value_ordinal(&placement,
                                                               ordinal);
    ASSERT_EQ(defining, &placement.relations[range.start]);
    EXPECT_EQ(defining->op, transfer);
    EXPECT_EQ(placement.relations[range.start + 1].op, use);
    for (auto value : {source, peer, forwarded, received}) {
      EXPECT_EQ(
          loom_low_placement_defining_transfer_for_value_ordinal(
              &placement, loom_local_value_domain_try_ordinal(&domain, value)),
          nullptr);
    }
    for (loom_value_ordinal_t i = 0; i < placement.value_count; ++i) {
      const auto outgoing = placement.ranges_by_source_ordinal[i];
      for (uint32_t j = 0; j < outgoing.count; ++j) {
        const auto index =
            placement.relation_indices_by_source_ordinal[outgoing.start + j];
        EXPECT_EQ(placement.relations[index].source_ordinal, i);
      }
    }
    ASSERT_EQ(placement.edge_relation_count, 2u);
    EXPECT_EQ(placement.relations[placement.edge_relation_indices[0]].op,
              entry_branch);
    EXPECT_EQ(placement.relations[placement.edge_relation_indices[1]].op,
              producer_branch);
    loom_local_value_domain_release(&domain);
    loom_module_free(module);
  }
  loom_context_deinitialize(&context);
  iree_arena_block_pool_deinitialize(&pool);
}

TEST(LowPlacementTest, StorageCompositionDoesNotImplyBitIdentity) {
  loom_low_placement_relation_t write = {};
  write.source_ordinal = 0;
  write.result_ordinal = 1;
  write.unit_count = 2;
  write.cause = LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT;
  write.flags = LOOM_LOW_PLACEMENT_RELATION_FLAG_HARD |
                LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE |
                LOOM_LOW_PLACEMENT_RELATION_FLAG_WRITES_STORAGE;
  loom_low_placement_relation_t edge = {};
  edge.source_ordinal = 1;
  edge.result_ordinal = 2;
  edge.unit_count = 2;
  edge.cause = LOOM_LOW_PLACEMENT_CAUSE_LOW_BRANCH;
  edge.flags = LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE |
               LOOM_LOW_PLACEMENT_RELATION_FLAG_IDENTITY_EDGE;

  loom_low_placement_relation_t composed = {};
  ASSERT_TRUE(loom_low_placement_relation_compose(&write, &edge, &composed));
  EXPECT_EQ(composed.source_ordinal, 0u);
  EXPECT_EQ(composed.result_ordinal, 2u);
  EXPECT_EQ(composed.flags, LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE);
}

TEST(LowPlacementTest, RetainsOperandConstraintsAcrossExactTiesOnly) {
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
  classes[0].flags = LOOM_LOW_REG_CLASS_FLAG_PHYSICAL;
  classes[0].allocatable_count = 7;
  const loom_low_reg_class_alt_t alternatives[] = {
      {0, LOOM_LOW_REGISTER_PART_NONE, LOOM_LOW_REG_CLASS_ALT_FLAG_PREFERRED,
       0},
      {1, LOOM_LOW_REGISTER_PART_NONE, 0, 0},
      {0, LOOM_LOW_REGISTER_PART_NONE, LOOM_LOW_REG_CLASS_ALT_FLAG_PREFERRED,
       3},
      {1, LOOM_LOW_REGISTER_PART_NONE, 0, 1},
  };
  loom_low_operand_t operands[3] = {};
  for (auto& operand : operands) {
    operand.role = LOOM_LOW_OPERAND_ROLE_OPERAND;
    operand.reg_class_alt_count = 2;
    operand.unit_count = 1;
  }
  operands[0].role = LOOM_LOW_OPERAND_ROLE_RESULT;
  operands[0].address_map_kind = LOOM_LOW_OPERAND_ADDRESS_MAP_LOW_SUBSET;
  operands[0].addressable_unit_count = 16;
  operands[1].address_map_kind = LOOM_LOW_OPERAND_ADDRESS_MAP_TARGET_STATE;
  operands[1].addressable_unit_count = 16;
  operands[1].address_state_slot = 1;
  operands[2].reg_class_alt_start = 2;
  operands[2].address_map_kind = LOOM_LOW_OPERAND_ADDRESS_MAP_LOW_SUBSET;
  operands[2].addressable_unit_count = 8;
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

  const loom_low_placement_value_ref_t values[] = {
      {0, LOOM_LOW_PLACEMENT_VALUE_RESULT, 0},
      {0, LOOM_LOW_PLACEMENT_VALUE_OPERAND, 0},
      {0, LOOM_LOW_PLACEMENT_VALUE_OPERAND, 0}};
  loom_low_placement_predicate_t predicate = {
      0, 1, 0, 0, 1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 1};
  const loom_low_placement_clause_t clause = {0, 1, 1,
                                              LOOM_LOW_PLACEMENT_CLAUSE_ANY};
  const loom_low_placement_preference_t instruction_preferences[] = {
      {values, &predicate, &clause, 3, 1},
      {values + 1, &predicate, &clause, 2, 1}};
  const uint16_t preference_indices[] = {1, 2};

  struct PreferenceCase {
    // Selected descriptor class: finite physical zero or unbounded virtual one.
    uint16_t class_id;
    // Actual predicate semantics, independent of allocation strategy.
    loom_low_placement_relation_kind_t kind;
    // Original predicate mask, potentially noncontiguous or full-width.
    uint32_t location_mask;
    // Expected carry-closed dependency mask.
    uint32_t dependency_mask;
    // Expected power-of-two domain cap, or zero for direct evaluation.
    uint32_t entry_count;
  };
  const PreferenceCase cases[] = {
      {0, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 1, 1, 2},
      {0, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 5, 7, 4},
      {0, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, UINT32_MAX,
       UINT32_MAX, 4},
      {1, LOOM_LOW_PLACEMENT_RELATION_DIFFERENT_MASKED_LOCATION, 5, 7, 0},
      {0, LOOM_LOW_PLACEMENT_RELATION_DISJOINT_STORAGE, 0, 0, 0},
  };
  for (const auto& test_case : cases) {
    const uint16_t class_id = test_case.class_id;
    predicate.kind = test_case.kind;
    predicate.location_mask = test_case.location_mask;
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
    loom_low_placement_preference_index_t preferences = {};
    IREE_ASSERT_OK(loom_low_placement_analyze_region(
        module, body, &descriptor_set, &domain, &liveness, {},
        {preference_indices, instruction_preferences}, &module->arena,
        &module->arena, &placement, &preferences));
    ASSERT_EQ(preferences.use_count, 3u);
    ASSERT_EQ(preferences.binding_count, 8u);
    EXPECT_EQ(preferences.instruction_use_count, 3u);
    EXPECT_EQ(preferences.max_incident_use_count, 0u);
    EXPECT_EQ(preferences.max_incident_binding_count, 0u);
    EXPECT_EQ(preferences.max_memo_entry_count, 0u);
    EXPECT_EQ(preferences.offsets_by_origin, nullptr);
    EXPECT_EQ(preferences.use_indices, nullptr);
    const auto origin = loom_local_value_domain_ordinal(&domain, chain[0]);
    for (uint32_t i = 0; i < preferences.use_count; ++i) {
      const auto& use = preferences.uses[i];
      EXPECT_EQ(use.memo.location_bit_count == 0
                    ? 0
                    : UINT32_MAX >> (32 - use.memo.location_bit_count),
                test_case.dependency_mask);
      if (test_case.entry_count == 0) {
        EXPECT_EQ(use.memo.index_bit_count_plus_one, 0u);
      } else {
        EXPECT_EQ(UINT32_C(1) << (use.memo.index_bit_count_plus_one - 1),
                  test_case.entry_count);
      }
      for (uint16_t j = 0; j < use.preference->value_count; ++j) {
        const auto& binding = preferences.bindings[use.binding_start + j];
        EXPECT_EQ(binding.representative, j);
        EXPECT_EQ(
            placement
                .tied_storage_origins_by_value_ordinal[binding.value_ordinal],
            origin);
      }
    }
    // Deferred instruction bindings keep actual values without an origin CSR.
    EXPECT_EQ(preferences.bindings[0].value_ordinal,
              loom_local_value_domain_ordinal(&domain, chain[1]));
    EXPECT_EQ(preferences.bindings[1].value_ordinal, origin);
    ASSERT_NE(placement.operand_constraints_by_interval, nullptr);
    ASSERT_NE(placement.tied_storage_origins_by_value_ordinal, nullptr);
    const auto chain_origin =
        loom_local_value_domain_try_ordinal(&domain, chain[0]);
    for (auto value : chain) {
      const auto ordinal = loom_local_value_domain_try_ordinal(&domain, value);
      EXPECT_EQ(placement.tied_storage_origins_by_value_ordinal[ordinal],
                chain_origin);
      EXPECT_EQ(placement
                    .operand_constraints_by_interval
                        [liveness.value_interval_indices[ordinal]]
                    .unit_alignment_log2,
                class_id == 0 ? 3 : 1);
      EXPECT_EQ(placement
                    .operand_constraints_by_interval
                        [liveness.value_interval_indices[ordinal]]
                    .addressable_unit_count,
                8u);
      EXPECT_TRUE(placement
                      .operand_constraints_by_interval
                          [liveness.value_interval_indices[ordinal]]
                      .has_target_address_state);
    }
    const auto source_ordinal =
        loom_local_value_domain_try_ordinal(&domain, source);
    EXPECT_EQ(placement.tied_storage_origins_by_value_ordinal[source_ordinal],
              source_ordinal);
    EXPECT_EQ(placement
                  .operand_constraints_by_interval
                      [liveness.value_interval_indices[source_ordinal]]
                  .unit_alignment_log2,
              0);
    EXPECT_EQ(placement
                  .operand_constraints_by_interval
                      [liveness.value_interval_indices[source_ordinal]]
                  .addressable_unit_count,
              0u);
    EXPECT_FALSE(placement
                     .operand_constraints_by_interval
                         [liveness.value_interval_indices[source_ordinal]]
                     .has_target_address_state);
    loom_local_value_domain_release(&domain);
    loom_module_free(module);
  }
  loom_context_deinitialize(&context);
  iree_arena_block_pool_deinitialize(&pool);
}

}  // namespace
}  // namespace loom
