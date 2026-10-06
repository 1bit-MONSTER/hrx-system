// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/placement_capture.h"

#include <string.h>

#include "loom/codegen/low/placement.h"
#include "loom/ops/low/ops.h"

typedef struct loom_low_placement_capture_writes_t {
  // Write-point range starts by required storage origin, with a sentinel.
  uint32_t* offsets;
  // Sorted mandatory write points within each origin's range.
  uint32_t* points;
} loom_low_placement_capture_writes_t;

static loom_value_ordinal_t loom_low_placement_capture_storage_origin(
    const loom_low_placement_table_t* placement, loom_value_ordinal_t value) {
  return placement->tied_storage_origins_by_value_ordinal != NULL
             ? placement->tied_storage_origins_by_value_ordinal[value]
             : value;
}

static iree_status_t loom_low_placement_capture_writes_initialize(
    const loom_low_placement_table_t* placement, iree_arena_allocator_t* arena,
    loom_low_placement_capture_writes_t* out_writes) {
  *out_writes = (loom_low_placement_capture_writes_t){0};
  const uint32_t write_count = placement->storage.write_relation_count;
  const iree_host_size_t offset_count =
      (iree_host_size_t)placement->value_count + 1;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, offset_count,
                                                 sizeof(*out_writes->offsets),
                                                 (void**)&out_writes->offsets));
  memset(out_writes->offsets, 0, offset_count * sizeof(*out_writes->offsets));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, write_count,
                                                 sizeof(*out_writes->points),
                                                 (void**)&out_writes->points));
  for (uint32_t i = 0; i < write_count; ++i) {
    const loom_low_placement_relation_t* relation =
        &placement->relations[placement->storage.write_relation_indices[i]];
    const loom_value_ordinal_t origin =
        loom_low_placement_capture_storage_origin(placement,
                                                  relation->source_ordinal);
    ++out_writes->offsets[origin + 1];
  }
  for (loom_value_ordinal_t v = 0; v < placement->value_count; ++v) {
    out_writes->offsets[v + 1] += out_writes->offsets[v];
  }
  // The producer retained write-point order before grouping relations by
  // result. Stable distribution into origin ranges preserves that order without
  // sorting.
  for (uint32_t i = 0; i < write_count; ++i) {
    const loom_low_placement_relation_t* relation =
        &placement->relations[placement->storage.write_relation_indices[i]];
    const loom_value_ordinal_t origin =
        loom_low_placement_capture_storage_origin(placement,
                                                  relation->source_ordinal);
    out_writes->points[out_writes->offsets[origin]++] = relation->write_point;
  }
  for (loom_value_ordinal_t v = placement->value_count; v > 0; --v) {
    out_writes->offsets[v] = out_writes->offsets[v - 1];
  }
  out_writes->offsets[0] = 0;
  return iree_ok_status();
}

static bool loom_low_placement_capture_required(
    const loom_liveness_analysis_t* liveness,
    const loom_low_placement_capture_writes_t* writes,
    loom_value_ordinal_t aggregate, loom_value_ordinal_t source_origin) {
  if (writes->offsets == NULL) {
    return false;
  }
  const uint32_t begin = writes->offsets[source_origin];
  const uint32_t end = writes->offsets[source_origin + 1];
  if (begin == end) {
    return false;
  }
  const loom_liveness_segment_range_t range =
      loom_liveness_segment_range_for_value_ordinal(liveness, aggregate);
  for (uint32_t i = 0; i < range.count; ++i) {
    const loom_liveness_segment_t* segment =
        &liveness->segments[range.start + i];
    uint32_t first = begin;
    uint32_t count = end - begin;
    while (count != 0) {
      const uint32_t half = count / 2;
      const uint32_t middle = first + half;
      if (writes->points[middle] < segment->start_point) {
        first = middle + 1;
        count -= half + 1;
      } else {
        count = half;
      }
    }
    if (first != end && writes->points[first] < segment->end_point) {
      return true;
    }
  }
  return false;
}

static bool loom_low_placement_capture_has_packet_uses(
    const loom_low_placement_table_t* placement, loom_value_ordinal_t value) {
  const loom_value_t* result =
      loom_module_value(placement->module, placement->value_ids[value]);
  const loom_use_t* use = NULL;
  loom_value_for_each_use(result, use) {
    if (!loom_low_br_isa(loom_use_user_op(*use))) {
      return true;
    }
  }
  return false;
}

static bool loom_low_placement_capture_has_edge_uses(
    const loom_low_placement_table_t* placement, loom_value_ordinal_t value) {
  const loom_low_placement_relation_range_t range =
      loom_low_placement_relation_range_for_source_value_ordinal(placement,
                                                                 value);
  for (uint32_t i = 0; i < range.count; ++i) {
    const loom_low_placement_relation_t* relation =
        &placement->relations
             [placement->relation_indices_by_source_ordinal[range.start + i]];
    if (relation->source_ordinal == value &&
        loom_low_placement_cause_is_edge(relation->cause)) {
      return true;
    }
  }
  return false;
}

iree_status_t loom_low_placement_captures_build(
    const loom_liveness_analysis_t* liveness,
    loom_low_placement_table_t* placement, iree_arena_allocator_t* arena,
    iree_arena_allocator_t* scratch_arena) {
  if (!iree_any_bit_set(placement->storage.flags,
                        LOOM_LOW_PLACEMENT_STORAGE_FLAG_CONCAT)) {
    return iree_ok_status();
  }
  loom_low_placement_capture_writes_t writes = {0};
  loom_low_placement_capture_t* captures = NULL;
  iree_host_size_t capture_capacity = 0;
  uint32_t capture_count = 0;
  uint32_t* last_captures = NULL;
  loom_value_ordinal_t aggregate = LOOM_VALUE_ORDINAL_INVALID;
  bool has_packet_uses = false;
  bool has_edge_uses = false;
  for (uint32_t i = 0; i < placement->relation_count; ++i) {
    loom_low_placement_relation_t* relation = &placement->relations[i];
    if (relation->cause != LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT) {
      continue;
    }
    if (aggregate != relation->result_ordinal) {
      aggregate = relation->result_ordinal;
      has_packet_uses =
          loom_low_placement_capture_has_packet_uses(placement, aggregate);
      has_edge_uses =
          loom_low_placement_capture_has_edge_uses(placement, aggregate);
      if (has_edge_uses && writes.offsets == NULL &&
          placement->storage.write_relation_count != 0) {
        IREE_RETURN_IF_ERROR(loom_low_placement_capture_writes_initialize(
            placement, scratch_arena, &writes));
      }
    }
    if (has_packet_uses) {
      relation->flags |= LOOM_LOW_PLACEMENT_RELATION_FLAG_MATERIALIZE_PART;
    }
    const loom_value_ordinal_t origin =
        loom_low_placement_capture_storage_origin(placement,
                                                  relation->source_ordinal);
    if (!has_edge_uses || !loom_low_placement_capture_required(
                              liveness, &writes, aggregate, origin)) {
      continue;
    }
    if (last_captures == NULL) {
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          scratch_arena, placement->value_count, sizeof(*last_captures),
          (void**)&last_captures));
      memset(last_captures, 0xFF,
             placement->value_count * sizeof(*last_captures));
    }
    uint32_t unit_offset = relation->result_unit_offset;
    const uint32_t previous = last_captures[origin];
    if (!has_packet_uses && previous != UINT32_MAX &&
        placement->relations[captures[previous].relation_index]
                .result_ordinal == aggregate) {
      unit_offset = captures[previous].unit_offset;
      // This duplicate has no storage of its own. Its edge transport reads
      // the saved representative instead of coalescing with the source.
      relation->flags &= ~LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE;
    } else {
      relation->flags |= LOOM_LOW_PLACEMENT_RELATION_FLAG_MATERIALIZE_PART;
    }
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        scratch_arena, capture_count, capture_count + 1, sizeof(*captures),
        &capture_capacity, (void**)&captures));
    captures[capture_count] = (loom_low_placement_capture_t){
        .relation_index = i,
        .unit_offset = unit_offset,
    };
    last_captures[origin] = capture_count++;
    relation->flags |= LOOM_LOW_PLACEMENT_RELATION_FLAG_CAPTURED_PART;
  }
  if (capture_count != 0) {
    loom_low_placement_capture_t* retained_captures = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, capture_count,
                                                   sizeof(*retained_captures),
                                                   (void**)&retained_captures));
    memcpy(retained_captures, captures,
           capture_count * sizeof(*retained_captures));
    placement->captures = retained_captures;
    placement->capture_count = capture_count;
  }
  return iree_ok_status();
}

loom_low_placement_concat_source_t loom_low_placement_concat_source(
    const loom_low_placement_table_t* placement, uint32_t relation_index) {
  const loom_low_placement_relation_t* relation =
      &placement->relations[relation_index];
  if (!iree_any_bit_set(relation->flags,
                        LOOM_LOW_PLACEMENT_RELATION_FLAG_CAPTURED_PART)) {
    return (loom_low_placement_concat_source_t){
        .value_ordinal = relation->source_ordinal,
        .unit_offset = relation->source_unit_offset,
    };
  }
  uint32_t first = 0;
  uint32_t count = placement->capture_count;
  while (count != 0) {
    const uint32_t half = count / 2;
    const uint32_t middle = first + half;
    if (placement->captures[middle].relation_index < relation_index) {
      first = middle + 1;
      count -= half + 1;
    } else {
      count = half;
    }
  }
  return (loom_low_placement_concat_source_t){
      .value_ordinal = relation->result_ordinal,
      .unit_offset = placement->captures[first].unit_offset,
  };
}
