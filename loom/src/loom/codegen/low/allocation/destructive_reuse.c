// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/destructive_reuse.h"

#include <string.h>

static bool loom_low_allocation_reuse_relation(
    const loom_low_placement_relation_t* relation) {
  return loom_low_placement_relation_can_alias(relation) &&
         relation->cause >= LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT &&
         relation->cause <= LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT;
}

// Non-writing required aliases denote one content version. Their first-write
// queries share the existing per-unit slots, while destructive tied results
// retain separate slots for the new contents they produce.
static iree_status_t loom_low_allocation_content_unit_starts_build(
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    const loom_low_placement_table_t* placement,
    iree_arena_allocator_t* scratch, uint32_t** out_unit_starts) {
  uint32_t* unit_starts = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(scratch, placement->value_count,
                                sizeof(*unit_starts), (void**)&unit_starts));
  for (loom_value_ordinal_t cursor = placement->storage_value_order_count;
       cursor > 0; --cursor) {
    const loom_value_ordinal_t value =
        placement->storage_value_order[cursor - 1];
    unit_starts[value] = unit_liveness->values[value].unit_point_start;
    const loom_low_placement_relation_range_t range =
        placement->ranges_by_result_ordinal[value];
    for (uint32_t i = 0; i < range.count; ++i) {
      const loom_low_placement_relation_t* relation =
          &placement->relations[range.start + i];
      if (relation->cause == LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT &&
          !iree_any_bit_set(relation->flags,
                            LOOM_LOW_PLACEMENT_RELATION_FLAG_WRITES_STORAGE)) {
        unit_starts[relation->result_ordinal] =
            unit_starts[relation->source_ordinal];
      }
    }
  }
  *out_unit_starts = unit_starts;
  return iree_ok_status();
}

static uint32_t loom_low_allocation_content_unit_start(
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    const uint32_t* content_unit_starts, loom_value_ordinal_t value_ordinal) {
  return content_unit_starts != NULL
             ? content_unit_starts[value_ordinal]
             : unit_liveness->values[value_ordinal].unit_point_start;
}

// Returns true when any mapped storage component remains observable at its
// counterpart's first write. Equal adjacent write points share one indexed
// component query, keeping wide relations proportional to mapped units.
static bool loom_low_allocation_storage_observed_at_first_writes(
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    const loom_liveness_analysis_t* liveness,
    const loom_low_placement_table_t* placement,
    loom_value_ordinal_t observed_ordinal, uint32_t observed_unit_offset,
    const uint32_t* first_writes, uint32_t write_unit_start,
    uint32_t unit_count) {
  uint32_t unit = 0;
  while (unit < unit_count) {
    const uint32_t write_point = first_writes[write_unit_start + unit];
    uint32_t run_count = 1;
    while (run_count < unit_count - unit &&
           first_writes[write_unit_start + unit + run_count] == write_point) {
      ++run_count;
    }
    if (write_point != UINT32_MAX &&
        loom_low_allocation_unit_liveness_storage_component_live_at_point(
            unit_liveness, liveness, placement, observed_ordinal,
            observed_unit_offset + unit, run_count, write_point)) {
      return true;
    }
    unit += run_count;
  }
  return false;
}

static iree_status_t loom_low_allocation_refine_destructive_reuse_build(
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    const loom_liveness_analysis_t* liveness,
    loom_low_placement_table_t* placement, bool has_identity_aliases,
    iree_arena_allocator_t* scratch) {
  uint32_t* content_unit_starts = NULL;
  if (has_identity_aliases) {
    IREE_RETURN_IF_ERROR(loom_low_allocation_content_unit_starts_build(
        unit_liveness, placement, scratch, &content_unit_starts));
  }
  uint32_t* first_writes = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(scratch, unit_liveness->point_count,
                                sizeof(*first_writes), (void**)&first_writes));
  memset(first_writes, 0xFF,
         unit_liveness->point_count * sizeof(*first_writes));
  for (uint32_t i = 0; i < placement->storage.write_relation_count; ++i) {
    const loom_low_placement_relation_t* relation =
        &placement->relations[placement->storage.write_relation_indices[i]];
    const uint32_t source_start =
        loom_low_allocation_content_unit_start(
            unit_liveness, content_unit_starts, relation->source_ordinal) +
        relation->source_unit_offset;
    for (uint32_t unit = 0; unit < relation->unit_count; ++unit) {
      first_writes[source_start + unit] =
          iree_min(first_writes[source_start + unit], relation->write_point);
    }
  }

  const loom_value_ordinal_t* order = placement->storage_value_order;
  const loom_value_ordinal_t order_count = placement->storage_value_order_count;
  IREE_ASSERT_EQ(order_count, placement->value_count);

  // Retain the first possible write through each optional identity path. A
  // materialized relation cuts that path, so its sources do not inherit the
  // result's writes. Each relation and its unit mapping are visited once.
  for (loom_value_ordinal_t cursor = 0; cursor < order_count; ++cursor) {
    const loom_low_placement_relation_range_t range =
        placement->ranges_by_result_ordinal[order[cursor]];
    for (uint32_t i = 0; i < range.count; ++i) {
      loom_low_placement_relation_t* relation =
          &placement->relations[range.start + i];
      if (!loom_low_allocation_reuse_relation(relation)) {
        continue;
      }
      const uint32_t source_start =
          loom_low_allocation_content_unit_start(
              unit_liveness, content_unit_starts, relation->source_ordinal) +
          relation->source_unit_offset;
      const uint32_t result_start =
          loom_low_allocation_content_unit_start(
              unit_liveness, content_unit_starts, relation->result_ordinal) +
          relation->result_unit_offset;
      if (source_start == result_start) {
        continue;
      }
      bool requires_copy = false;
      if (relation->cause != LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT) {
        requires_copy = loom_low_allocation_storage_observed_at_first_writes(
            unit_liveness, liveness, placement, relation->source_ordinal,
            relation->source_unit_offset, first_writes, result_start,
            relation->unit_count);
        if (!requires_copy) {
          requires_copy = loom_low_allocation_storage_observed_at_first_writes(
              unit_liveness, liveness, placement, relation->result_ordinal,
              relation->result_unit_offset, first_writes, source_start,
              relation->unit_count);
        }
      }
      if (requires_copy) {
        relation->flags &= ~LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE;
        continue;
      }
      for (uint32_t unit = 0; unit < relation->unit_count; ++unit) {
        first_writes[source_start + unit] =
            iree_min(first_writes[source_start + unit],
                     first_writes[result_start + unit]);
      }
    }
  }
  return iree_ok_status();
}

iree_status_t loom_low_allocation_refine_destructive_reuse(
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    const loom_liveness_analysis_t* liveness,
    loom_low_placement_table_t* placement, iree_arena_allocator_t* arena) {
  if (placement->storage.write_relation_count == 0 ||
      !iree_any_bit_set(placement->storage.flags,
                        LOOM_LOW_PLACEMENT_STORAGE_FLAG_OPTIONAL_ALIASES)) {
    return iree_ok_status();
  }
  iree_arena_allocator_t scratch;
  iree_arena_initialize(arena->block_pool, &scratch);
  const bool has_identity_aliases =
      iree_any_bit_set(placement->storage.flags,
                       LOOM_LOW_PLACEMENT_STORAGE_FLAG_IDENTITY_ALIASES);
  iree_status_t status = loom_low_allocation_refine_destructive_reuse_build(
      unit_liveness, liveness, placement, has_identity_aliases, &scratch);
  iree_arena_deinitialize(&scratch);
  return status;
}
