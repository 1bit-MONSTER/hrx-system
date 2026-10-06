// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/storage_liveness_index.h"

#include <string.h>

#include "loom/codegen/low/allocation/live_range.h"
#include "loom/codegen/low/allocation/storage.h"
#include "loom/codegen/low/allocation/unit_location.h"

struct loom_low_allocation_storage_lifetime_t {
  // Descriptor-defined namespace shared by aliasing register classes.
  uint32_t storage_key;
  // Atomic location within the storage namespace.
  uint32_t location;
  // Inclusive lifetime start point.
  uint32_t start_point;
  // Maximum exclusive end among earlier records with this storage identity.
  uint32_t prefix_maximum_end_point;
};

static uint32_t loom_low_allocation_storage_liveness_kind_ordinal(
    loom_low_allocation_location_kind_t location_kind) {
  IREE_ASSERT(
      loom_low_allocation_location_kind_is_register_like(location_kind));
  return location_kind == LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER ? 0
                                                                         : 1;
}

static loom_low_allocation_assignment_t
loom_low_allocation_storage_liveness_unit_assignment(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_allocation_assignment_t* assignment, uint32_t unit_index) {
  const loom_low_move_location_t location =
      loom_low_allocation_assignment_unit_location(descriptor_set, assignment,
                                                   unit_index);
  return (loom_low_allocation_assignment_t){
      .descriptor_reg_class_id = location.descriptor_reg_class_id,
      .location_kind = location.location_kind,
      .location_base = location.location,
      .location_count = 1,
  };
}

static iree_status_t loom_low_allocation_storage_liveness_count_records(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_allocation_assignment_t* assignments,
    iree_host_size_t assignment_count,
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    iree_host_size_t out_record_counts[2]) {
  out_record_counts[0] = 0;
  out_record_counts[1] = 0;
  for (iree_host_size_t i = 0; i < assignment_count; ++i) {
    const loom_low_allocation_assignment_t* assignment = &assignments[i];
    if (!loom_low_allocation_assignment_is_register_like(assignment)) {
      continue;
    }
    const uint32_t kind_ordinal =
        loom_low_allocation_storage_liveness_kind_ordinal(
            assignment->location_kind);
    for (uint32_t unit_index = 0; unit_index < assignment->location_count;
         ++unit_index) {
      const uint32_t start_point =
          loom_low_allocation_live_range_assignment_unit_start_point(
              unit_liveness->start_points, unit_liveness->point_count,
              assignment, unit_index);
      const uint32_t end_point =
          loom_low_allocation_live_range_assignment_unit_end_point(
              unit_liveness->end_points, unit_liveness->point_count, assignment,
              unit_index);
      if (start_point >= end_point) {
        continue;
      }
      const loom_low_allocation_assignment_t unit_assignment =
          loom_low_allocation_storage_liveness_unit_assignment(
              descriptor_set, assignment, unit_index);
      const uint32_t atomic_unit_count =
          loom_low_allocation_storage_assignment_atomic_unit_count(
              descriptor_set, &unit_assignment);
      if (!iree_host_size_checked_add(out_record_counts[kind_ordinal],
                                      atomic_unit_count,
                                      &out_record_counts[kind_ordinal])) {
        return iree_make_status(
            IREE_STATUS_RESOURCE_EXHAUSTED,
            "storage liveness record count exceeds host size");
      }
    }
  }
  return iree_ok_status();
}

static uint32_t loom_low_allocation_storage_liveness_sort_word(
    const loom_low_allocation_storage_lifetime_t* lifetime,
    uint32_t word_ordinal) {
  switch (word_ordinal) {
    case 0:
      return lifetime->start_point;
    case 1:
      return lifetime->location;
    default:
      return lifetime->storage_key;
  }
}

// Performs stable least-significant-digit passes over start, location, then
// storage key. Constant byte lanes do not affect order and are skipped after
// one linear inspection per word.
static void loom_low_allocation_storage_liveness_radix_sort(
    loom_low_allocation_storage_lifetime_t* records,
    loom_low_allocation_storage_lifetime_t* temporary, uint32_t record_count) {
  if (record_count <= 1) {
    return;
  }
  loom_low_allocation_storage_lifetime_t* source = records;
  loom_low_allocation_storage_lifetime_t* destination = temporary;
  for (uint32_t word_ordinal = 0; word_ordinal < 3; ++word_ordinal) {
    const uint32_t reference = loom_low_allocation_storage_liveness_sort_word(
        &source[0], word_ordinal);
    uint32_t varying_bits = 0;
    for (uint32_t i = 1; i < record_count; ++i) {
      varying_bits |=
          reference ^ loom_low_allocation_storage_liveness_sort_word(
                          &source[i], word_ordinal);
    }
    for (uint32_t shift = 0; shift < 32; shift += 8) {
      if (((varying_bits >> shift) & 0xFFu) == 0) {
        continue;
      }
      uint32_t offsets[256] = {0};
      for (uint32_t i = 0; i < record_count; ++i) {
        ++offsets[(loom_low_allocation_storage_liveness_sort_word(
                       &source[i], word_ordinal) >>
                   shift) &
                  0xFFu];
      }
      uint32_t next_offset = 0;
      for (uint32_t i = 0; i < IREE_ARRAYSIZE(offsets); ++i) {
        const uint32_t count = offsets[i];
        offsets[i] = next_offset;
        next_offset += count;
      }
      for (uint32_t i = 0; i < record_count; ++i) {
        destination[offsets[(loom_low_allocation_storage_liveness_sort_word(
                                 &source[i], word_ordinal) >>
                             shift) &
                            0xFFu]++] = source[i];
      }
      loom_low_allocation_storage_lifetime_t* swap = source;
      source = destination;
      destination = swap;
    }
  }
  if (source != records) {
    memcpy(records, source, record_count * sizeof(*records));
  }
}

iree_status_t loom_low_allocation_storage_liveness_index_initialize(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_allocation_assignment_t* assignments,
    iree_host_size_t assignment_count,
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    iree_arena_allocator_t* arena,
    loom_low_allocation_storage_liveness_index_t* out_index) {
  *out_index = (loom_low_allocation_storage_liveness_index_t){0};
  iree_host_size_t record_counts[2];
  IREE_RETURN_IF_ERROR(loom_low_allocation_storage_liveness_count_records(
      descriptor_set, assignments, assignment_count, unit_liveness,
      record_counts));
  iree_host_size_t record_count = 0;
  if (!iree_host_size_checked_add(record_counts[0], record_counts[1],
                                  &record_count) ||
      record_count > UINT32_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "storage liveness index exceeds uint32_t range");
  }
  out_index->lifetime_starts[1] = (uint32_t)record_counts[0];
  out_index->lifetime_counts[0] = (uint32_t)record_counts[0];
  out_index->lifetime_counts[1] = (uint32_t)record_counts[1];
  if (record_count == 0) {
    out_index->descriptor_set = descriptor_set;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, record_count, sizeof(*out_index->lifetimes),
      (void**)&out_index->lifetimes));

  iree_arena_allocator_t build_arena;
  iree_arena_initialize(arena->block_pool, &build_arena);
  loom_low_allocation_storage_lifetime_t* temporary = NULL;
  iree_status_t status = iree_arena_allocate_array(
      &build_arena, record_count, sizeof(*temporary), (void**)&temporary);
  uint32_t record_cursors[2] = {0, out_index->lifetime_starts[1]};
  for (iree_host_size_t i = 0;
       i < assignment_count && iree_status_is_ok(status); ++i) {
    const loom_low_allocation_assignment_t* assignment = &assignments[i];
    if (!loom_low_allocation_assignment_is_register_like(assignment)) {
      continue;
    }
    const uint32_t kind_ordinal =
        loom_low_allocation_storage_liveness_kind_ordinal(
            assignment->location_kind);
    for (uint32_t unit_index = 0; unit_index < assignment->location_count;
         ++unit_index) {
      const uint32_t start_point =
          loom_low_allocation_live_range_assignment_unit_start_point(
              unit_liveness->start_points, unit_liveness->point_count,
              assignment, unit_index);
      const uint32_t end_point =
          loom_low_allocation_live_range_assignment_unit_end_point(
              unit_liveness->end_points, unit_liveness->point_count, assignment,
              unit_index);
      if (start_point >= end_point) {
        continue;
      }
      const loom_low_allocation_assignment_t unit_assignment =
          loom_low_allocation_storage_liveness_unit_assignment(
              descriptor_set, assignment, unit_index);
      const uint32_t atomic_unit_count =
          loom_low_allocation_storage_assignment_atomic_unit_count(
              descriptor_set, &unit_assignment);
      for (uint32_t atomic_unit = 0; atomic_unit < atomic_unit_count;
           ++atomic_unit) {
        loom_low_allocation_storage_lifetime_t* lifetime =
            &out_index->lifetimes[record_cursors[kind_ordinal]++];
        loom_low_allocation_storage_assignment_atomic_unit(
            descriptor_set, &unit_assignment, atomic_unit,
            &lifetime->storage_key, &lifetime->location);
        lifetime->start_point = start_point;
        lifetime->prefix_maximum_end_point = end_point;
      }
    }
  }

  if (iree_status_is_ok(status)) {
    IREE_ASSERT_EQ(record_cursors[0], out_index->lifetime_counts[0]);
    IREE_ASSERT_EQ(record_cursors[1], record_count);
    for (uint32_t kind_ordinal = 0; kind_ordinal < 2; ++kind_ordinal) {
      const uint32_t start = out_index->lifetime_starts[kind_ordinal];
      const uint32_t count = out_index->lifetime_counts[kind_ordinal];
      loom_low_allocation_storage_liveness_radix_sort(
          out_index->lifetimes + start, temporary + start, count);
      uint32_t maximum_end_point = 0;
      for (uint32_t i = start; i < start + count; ++i) {
        loom_low_allocation_storage_lifetime_t* lifetime =
            &out_index->lifetimes[i];
        if (i == start ||
            lifetime->storage_key != out_index->lifetimes[i - 1].storage_key ||
            lifetime->location != out_index->lifetimes[i - 1].location) {
          maximum_end_point = 0;
        }
        maximum_end_point =
            iree_max(maximum_end_point, lifetime->prefix_maximum_end_point);
        lifetime->prefix_maximum_end_point = maximum_end_point;
      }
    }
    out_index->descriptor_set = descriptor_set;
  }
  iree_arena_deinitialize(&build_arena);
  return status;
}

static bool loom_low_allocation_storage_liveness_unit_is_live_at_point(
    const loom_low_allocation_storage_liveness_index_t* index,
    loom_low_allocation_location_kind_t location_kind, uint32_t storage_key,
    uint32_t location, uint32_t point) {
  const uint32_t kind_ordinal =
      loom_low_allocation_storage_liveness_kind_ordinal(location_kind);
  const uint32_t start = index->lifetime_starts[kind_ordinal];
  uint32_t begin = start;
  uint32_t end = start + index->lifetime_counts[kind_ordinal];
  while (begin < end) {
    const uint32_t middle = begin + (end - begin) / 2u;
    const loom_low_allocation_storage_lifetime_t* lifetime =
        &index->lifetimes[middle];
    if (lifetime->storage_key < storage_key ||
        (lifetime->storage_key == storage_key &&
         (lifetime->location < location || (lifetime->location == location &&
                                            lifetime->start_point <= point)))) {
      begin = middle + 1u;
    } else {
      end = middle;
    }
  }
  if (begin == start) {
    return false;
  }
  const loom_low_allocation_storage_lifetime_t* lifetime =
      &index->lifetimes[begin - 1u];
  return lifetime->storage_key == storage_key &&
         lifetime->location == location &&
         lifetime->prefix_maximum_end_point > point;
}

bool loom_low_allocation_storage_liveness_index_is_live_at_point(
    const loom_low_allocation_storage_liveness_index_t* index,
    const loom_low_move_location_t* location, uint32_t point) {
  const loom_low_allocation_assignment_t assignment = {
      .descriptor_reg_class_id = location->descriptor_reg_class_id,
      .location_kind = location->location_kind,
      .location_base = location->location,
      .location_count = 1,
  };
  const uint32_t atomic_unit_count =
      loom_low_allocation_storage_assignment_atomic_unit_count(
          index->descriptor_set, &assignment);
  for (uint32_t atomic_unit = 0; atomic_unit < atomic_unit_count;
       ++atomic_unit) {
    uint32_t storage_key = 0;
    uint32_t atomic_location = 0;
    loom_low_allocation_storage_assignment_atomic_unit(
        index->descriptor_set, &assignment, atomic_unit, &storage_key,
        &atomic_location);
    if (loom_low_allocation_storage_liveness_unit_is_live_at_point(
            index, location->location_kind, storage_key, atomic_location,
            point)) {
      return true;
    }
  }
  return false;
}
