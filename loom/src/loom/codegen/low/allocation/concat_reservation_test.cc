// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/concat_reservation.h"

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ir/types.h"

namespace loom {
namespace {

TEST(LowAllocationConcatReservationTest,
     DefaultAssemblyIncludesTheResultLifetime) {
  iree_arena_block_pool_t pool;
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool);
  iree_arena_allocator_t arena;
  iree_arena_initialize(&pool, &arena);
  loom_context_t ir_context;
  loom_context_initialize(iree_allocator_system(), &ir_context);
  IREE_CHECK_OK(loom_context_finalize(&ir_context));
  loom_module_t* module = nullptr;
  IREE_CHECK_OK(loom_module_allocate(&ir_context, IREE_SV("test"), &pool,
                                     nullptr, iree_allocator_system(),
                                     &module));
  loom_value_id_t value_ids[4];
  for (loom_value_id_t& value_id : value_ids) {
    IREE_CHECK_OK(loom_module_define_value(
        module, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), &value_id));
  }
  loom_module_value_ordinal_scratch_acquire(module);
  for (uint32_t i = 0; i < IREE_ARRAYSIZE(value_ids); ++i) {
    loom_module_value_ordinal_scratch_set(module, value_ids[i], i);
  }

  loom_liveness_value_class_t value_class = {};
  value_class.type_kind = LOOM_TYPE_REGISTER;
  value_class.register_descriptor_set_stable_id = 17;
  value_class.register_class_id = 0;
  loom_liveness_interval_t intervals[4] = {};
  for (uint32_t i = 0; i < IREE_ARRAYSIZE(intervals); ++i) {
    intervals[i].value_id = value_ids[i];
    intervals[i].value_class = value_class;
    intervals[i].unit_count = i == 2 ? 4 : 2;
  }
  intervals[0].start_point = 2;
  intervals[0].end_point = 4;
  intervals[1].start_point = 3;
  intervals[1].end_point = 4;
  intervals[2].start_point = 4;
  intervals[2].end_point = 10;
  const uint32_t interval_indices[] = {0, 1, 2, 3};
  loom_liveness_analysis_t liveness = {};
  liveness.intervals = intervals;
  liveness.interval_count = IREE_ARRAYSIZE(intervals);
  liveness.value_ids = value_ids;
  liveness.value_count = IREE_ARRAYSIZE(value_ids);
  liveness.value_interval_indices = interval_indices;

  loom_low_allocation_unit_liveness_value_t unit_values[] = {
      {0, 2}, {2, 3}, {4, 4}, {8, 5}};
  uint32_t unit_start_points[] = {2, 2, 3, 3, 2, 2, 3, 3, 5, 5};
  uint32_t unit_end_points[] = {4, 4, 4, 4, 10, 10, 10, 10, 14, 14};
  uint64_t incomplete_storage_words[] = {0};
  loom_low_allocation_unit_liveness_t unit_liveness = {};
  unit_liveness.values = unit_values;
  unit_liveness.start_points = unit_start_points;
  unit_liveness.end_points = unit_end_points;
  unit_liveness.point_count = IREE_ARRAYSIZE(unit_end_points);
  unit_liveness.values_with_incomplete_storage_segments = {
      liveness.value_count, incomplete_storage_words};

  loom_low_placement_relation_t relations[2] = {};
  for (uint32_t i = 0; i < IREE_ARRAYSIZE(relations); ++i) {
    relations[i].result_ordinal = 2;
    relations[i].source_ordinal = i;
    relations[i].result_unit_offset = i * 2;
    relations[i].unit_count = 2;
    relations[i].kind = LOOM_LOW_PLACEMENT_RELATION_CONTIGUOUS_PART;
    relations[i].cause = LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT;
    relations[i].flags = LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE;
  }
  const loom_low_placement_relation_range_t result_ranges[] = {
      {0, 0}, {0, 0}, {0, 2}, {2, 0}};
  const loom_low_placement_relation_range_t source_ranges[] = {
      {0, 1}, {1, 1}, {2, 0}, {2, 0}};
  const uint32_t source_relations[] = {0, 1};
  loom_low_placement_table_t placement = {};
  placement.value_ids = value_ids;
  placement.value_count = IREE_ARRAYSIZE(value_ids);
  placement.relations = relations;
  placement.relation_count = IREE_ARRAYSIZE(relations);
  placement.ranges_by_result_ordinal = result_ranges;
  placement.ranges_by_source_ordinal = source_ranges;
  placement.relation_indices_by_source_ordinal = source_relations;

  loom_low_reg_class_t reg_class = {};
  reg_class.flags = LOOM_LOW_REG_CLASS_FLAG_PHYSICAL;
  reg_class.alloc_unit_bits = 32;
  reg_class.allocatable_count = 16;
  reg_class.spill_class_id = LOOM_LOW_REG_CLASS_NONE;
  loom_low_descriptor_set_t descriptors = {};
  descriptors.stable_id = 17;
  descriptors.reg_classes = &reg_class;
  descriptors.reg_class_count = 1;
  loom_low_resolved_target_t target = {};
  target.descriptor_set = &descriptors;
  loom_low_allocation_resolved_reserved_range_t prefix = {};
  prefix.location_kind = LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER;
  prefix.location_count = 2;
  uint32_t high_water[] = {6};
  uint32_t constrained_high_water[] = {prefix.location_count};
  loom_low_allocation_target_constraints_t constraints = {};
  constraints.module = module;
  constraints.target = &target;
  constraints.reserved_ranges = &prefix;
  constraints.reserved_range_count = 1;
  constraints.max_assigned_location_end_by_reg_class = high_water;
  constraints.max_constrained_location_end_by_reg_class =
      constrained_high_water;

  loom_low_allocation_assignment_t future = {};
  future.value_id = value_ids[3];
  future.value_class = value_class;
  future.unit_count = 2;
  future.location_kind = LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER;
  future.location_count = 2;
  future.unit_point_start = 8;
  future.end_point = 14;
  const uint32_t assignment_indices[] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, 0};
  loom_low_allocation_assignment_map_t assignments = {};
  assignments.module = module;
  assignments.liveness = &liveness;
  assignments.assignments = &future;
  assignments.assignment_count = 1;
  assignments.assignment_indices_by_value_ordinal = assignment_indices;
  loom_low_allocation_storage_lease_state_t leases = {};
  loom_low_allocation_search_context_t context = {};
  context.module = module;
  context.descriptor_set = &descriptors;
  context.liveness = &liveness;
  context.unit_liveness = &unit_liveness;
  context.target_constraints = &constraints;
  context.assignment_map = &assignments;
  context.placement = &placement;
  context.storage_leases = &leases;

  // Both two-unit sources fit at 2 and 4 before the future reservation starts.
  // Their four-unit result must also fit from point 4 through point 10. A
  // reservation starting at 10 is disjoint and must not force preferred
  // four-unit alignment on the otherwise valid base-2 assembly.
  for (uint32_t future_base : {2u, 4u}) {
    for (uint32_t future_start : {5u, 10u}) {
      SCOPED_TRACE(::testing::Message() << "future base=" << future_base
                                        << " start=" << future_start);
      future.location_base = future_base;
      future.start_point = future_start;
      intervals[3].start_point = future_start;
      intervals[3].end_point = 14;
      unit_values[3].acquisition_start_point = future_start;
      unit_start_points[8] = unit_start_points[9] = future_start;
      loom_low_allocation_active_set_t active_set = {};
      IREE_CHECK_OK(loom_low_allocation_active_set_initialize(1, 16, 16, &arena,
                                                              &active_set));
      loom_low_allocation_active_set_insert(&active_set, &descriptors, &future,
                                            1, 0);
      context.active_set = &active_set;
      loom_low_allocation_assignment_t reservation;
      IREE_EXPECT_OK(loom_low_allocation_concat_reservation_find(
          &context, &intervals[0], &relations[0], &intervals[2],
          &result_ranges[2], value_ids, 2, &reservation));
      if (future_start == 5) {
        EXPECT_EQ(reservation.value_id, value_ids[2]);
        EXPECT_EQ(reservation.location_count, 4u);
        EXPECT_GE(reservation.location_base, prefix.location_count);
        EXPECT_TRUE(reservation.location_base + reservation.location_count <=
                        future_base ||
                    reservation.location_base >=
                        future_base + future.location_count);
      } else {
        EXPECT_EQ(reservation.value_id, LOOM_VALUE_ID_INVALID);
      }
      EXPECT_EQ(assignments.assignment_count, 1u);
    }
  }

  for (loom_value_id_t value_id : value_ids) {
    loom_module_value_ordinal_scratch_clear(module, value_id);
  }
  loom_module_value_ordinal_scratch_release(module);
  loom_module_free(module);
  iree_arena_deinitialize(&arena);
  loom_context_deinitialize(&ir_context);
  iree_arena_block_pool_deinitialize(&pool);
}

}  // namespace
}  // namespace loom
