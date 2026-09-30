// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/interval_order.h"

#include "loom/codegen/low/allocation/live_range.h"
#include "loom/util/adaptive_sort.h"

static bool loom_low_allocation_interval_order_less(
    const loom_low_allocation_interval_order_entry_t* lhs,
    const loom_low_allocation_interval_order_entry_t* rhs) {
  if (lhs->acquisition_start_point != rhs->acquisition_start_point) {
    return lhs->acquisition_start_point < rhs->acquisition_start_point;
  }
  if (lhs->topology_rank != rhs->topology_rank) {
    return lhs->topology_rank < rhs->topology_rank;
  }
  if (lhs->interval->end_point != rhs->interval->end_point) {
    return lhs->interval->end_point < rhs->interval->end_point;
  }
  return lhs->interval->value_id < rhs->interval->value_id;
}

LOOM_DEFINE_ADAPTIVE_SORT(loom_low_allocation_interval_order_sort,
                          loom_low_allocation_interval_order_entry_t,
                          loom_low_allocation_interval_order_less)

iree_status_t loom_low_allocation_interval_order_build(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_liveness_analysis_t* liveness,
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    const loom_low_placement_table_t* placement, iree_arena_allocator_t* arena,
    loom_low_allocation_interval_order_t* out_order) {
  *out_order = (loom_low_allocation_interval_order_t){0};
  iree_host_size_t interval_count = 0;
  for (iree_host_size_t i = 0; i < liveness->interval_count; ++i) {
    const loom_liveness_interval_t* interval = &liveness->intervals[i];
    if (loom_low_allocation_live_range_interval_is_allocatable(interval)) {
      ++interval_count;
    }
  }
  if (interval_count == 0) {
    return iree_ok_status();
  }

  loom_low_allocation_interval_order_entry_t* intervals = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, interval_count, sizeof(*intervals), (void**)&intervals));
  iree_host_size_t interval_index = 0;
  bool has_packable_aggregates = false;
  for (loom_value_ordinal_t rank = 0; rank < liveness->value_count; ++rank) {
    const loom_value_ordinal_t value_ordinal =
        placement->storage_value_order_count != 0
            ? placement->storage_value_order[liveness->value_count - rank - 1u]
            : rank;
    const loom_liveness_interval_t* interval =
        loom_liveness_interval_for_value_ordinal(liveness, value_ordinal);
    if (interval != NULL &&
        loom_low_allocation_live_range_interval_is_allocatable(interval)) {
      intervals[interval_index++] =
          (loom_low_allocation_interval_order_entry_t){
              .interval = interval,
              .acquisition_start_point =
                  unit_liveness->values[value_ordinal].acquisition_start_point,
              .topology_rank =
                  placement->tied_storage_origins_by_value_ordinal != NULL &&
                          placement->tied_storage_origins_by_value_ordinal
                                  [value_ordinal] != value_ordinal
                      ? rank + 1u
                      : 0,
          };
      if (interval->unit_count > 1 &&
          !loom_low_reg_class_uses_explicit_physical_registers(
              &descriptor_set
                   ->reg_classes[interval->value_class.register_class_id])) {
        has_packable_aggregates = true;
      }
    }
  }
  loom_low_allocation_interval_order_sort(intervals, interval_count);

  *out_order = (loom_low_allocation_interval_order_t){
      .intervals = intervals,
      .interval_count = interval_count,
      .has_packable_aggregates = has_packable_aggregates,
  };
  return iree_ok_status();
}
