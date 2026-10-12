// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_vector_partition.h"

#include <string.h>

#include "loom/util/fact_table.h"

struct loom_value_fact_vector_partitions_t {
  // Immutable partition records indexed by vector value ID; NULL is absent.
  const loom_value_fact_vector_partition_t** entries;
  // Allocated entry count.
  iree_host_size_t capacity;
};

static iree_status_t loom_value_fact_vector_partitions_ensure_capacity(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  if (!table->vector_partitions) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate(
        table->transient_arena, sizeof(*table->vector_partitions),
        (void**)&table->vector_partitions));
    *table->vector_partitions = (loom_value_fact_vector_partitions_t){0};
  }
  loom_value_fact_vector_partitions_t* partitions = table->vector_partitions;
  if (value_id < partitions->capacity) {
    return iree_ok_status();
  }
  const iree_host_size_t old_capacity = partitions->capacity;
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      table->transient_arena, old_capacity, (iree_host_size_t)value_id + 1,
      sizeof(*partitions->entries), &partitions->capacity,
      (void**)&partitions->entries));
  memset(partitions->entries + old_capacity, 0,
         (partitions->capacity - old_capacity) * sizeof(*partitions->entries));
  return iree_ok_status();
}

loom_value_fact_vector_partition_t loom_value_fact_table_query_vector_partition(
    const loom_value_fact_table_t* table, loom_value_id_t value_id) {
  const loom_value_fact_vector_partitions_t* partitions =
      table ? table->vector_partitions : NULL;
  if (!partitions || value_id >= partitions->capacity ||
      !partitions->entries[value_id]) {
    return (loom_value_fact_vector_partition_t){0};
  }
  return *partitions->entries[value_id];
}

void loom_value_fact_table_clear_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  loom_value_fact_vector_partitions_t* partitions = table->vector_partitions;
  if (partitions && value_id < partitions->capacity) {
    partitions->entries[value_id] = NULL;
  }
}

iree_status_t loom_value_fact_table_define_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_vector_partition_t partition) {
  if (partition.count == 0) {
    loom_value_fact_table_clear_vector_partition(table, value_id);
    return iree_ok_status();
  }
  IREE_ASSERT(partition.values != NULL);
  IREE_ASSERT_NE(partition.regular_lane_count, 0u);
  const loom_value_fact_vector_partition_t previous =
      loom_value_fact_table_query_vector_partition(table, value_id);
  const iree_host_size_t value_bytes =
      partition.count * sizeof(*partition.values);
  if (previous.count == partition.count &&
      previous.regular_lane_count == partition.regular_lane_count &&
      memcmp(previous.values, partition.values, value_bytes) == 0) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_value_fact_vector_partitions_ensure_capacity(table, value_id));
  loom_value_fact_vector_partition_t* storage = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(table->transient_arena,
                                           sizeof(*storage) + value_bytes,
                                           (void**)&storage));
  loom_value_id_t* values = (loom_value_id_t*)(storage + 1);
  memcpy(values, partition.values, value_bytes);
  *storage = (loom_value_fact_vector_partition_t){
      .values = values,
      .count = partition.count,
      .regular_lane_count = partition.regular_lane_count,
  };
  table->vector_partitions->entries[value_id] = storage;
  return iree_ok_status();
}

iree_status_t loom_value_fact_table_forward_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t source_value_id,
    loom_value_id_t result_value_id) {
  loom_value_fact_vector_partitions_t* partitions = table->vector_partitions;
  if (!partitions || source_value_id >= partitions->capacity ||
      !partitions->entries[source_value_id]) {
    loom_value_fact_table_clear_vector_partition(table, result_value_id);
    return iree_ok_status();
  }
  const loom_value_fact_vector_partition_t* partition =
      partitions->entries[source_value_id];
  IREE_RETURN_IF_ERROR(loom_value_fact_vector_partitions_ensure_capacity(
      table, result_value_id));
  table->vector_partitions->entries[result_value_id] = partition;
  return iree_ok_status();
}
