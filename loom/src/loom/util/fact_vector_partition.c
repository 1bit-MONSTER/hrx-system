// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_vector_partition.h"

#include <string.h>

#include "loom/util/fact_table.h"

enum {
  LOOM_VECTOR_PARTITION_EXACT = 1u << 0,
  LOOM_VECTOR_PARTITION_SNAPSHOT = 1u << 1,
};

typedef struct loom_value_fact_vector_partition_storage_t {
  // Component arrays follow this record in the same allocation.
  loom_value_fact_vector_partition_t partition;
  // Views whose components still have materializable SSA definitions.
  uint32_t available_views;
} loom_value_fact_vector_partition_storage_t;

typedef struct loom_value_fact_vector_partition_use_t {
  // Next retained component referencing the same SSA value.
  struct loom_value_fact_vector_partition_use_t* next;
  // Record owning the referenced component, possibly shared by value aliases.
  loom_value_fact_vector_partition_storage_t* storage;
  // Component index in the record's contiguous exact and snapshot arrays.
  iree_host_size_t component_index;
} loom_value_fact_vector_partition_use_t;

typedef struct loom_value_fact_vector_partition_entry_t {
  // Retained partition for this vector value, or NULL.
  loom_value_fact_vector_partition_storage_t* storage;
  // Retained component references to this SSA value.
  loom_value_fact_vector_partition_use_t* uses;
} loom_value_fact_vector_partition_entry_t;

struct loom_value_fact_vector_partitions_t {
  // Partitions and their component references indexed by SSA value ID.
  loom_value_fact_vector_partition_entry_t* entries;
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
      !partitions->entries[value_id].storage) {
    return (loom_value_fact_vector_partition_t){0};
  }
  const loom_value_fact_vector_partition_storage_t* storage =
      partitions->entries[value_id].storage;
  if (storage->available_views == 0) {
    return (loom_value_fact_vector_partition_t){0};
  }
  loom_value_fact_vector_partition_t partition = storage->partition;
  if (!(storage->available_views & LOOM_VECTOR_PARTITION_EXACT)) {
    partition.components = NULL;
    partition.count = 0;
    partition.regular_lane_count = 0;
  }
  if (!(storage->available_views & LOOM_VECTOR_PARTITION_SNAPSHOT)) {
    partition.snapshot_components = NULL;
    partition.snapshot_count = 0;
    partition.snapshot_lane_count = 0;
  }
  return partition;
}

static uint32_t loom_value_fact_vector_partition_use_views(
    const loom_value_fact_vector_partition_use_t* use) {
  const loom_value_fact_vector_partition_t* partition =
      &use->storage->partition;
  if (use->component_index >= partition->count) {
    return LOOM_VECTOR_PARTITION_SNAPSHOT;
  }
  return LOOM_VECTOR_PARTITION_EXACT |
         (partition->snapshot_components == partition->components
              ? LOOM_VECTOR_PARTITION_SNAPSHOT
              : 0);
}

iree_status_t loom_value_fact_table_replace_vector_partition_value(
    loom_value_fact_table_t* table, loom_value_id_t old_value,
    loom_value_id_t new_value) {
  loom_value_fact_vector_partitions_t* partitions = table->vector_partitions;
  if (!partitions || old_value >= partitions->capacity ||
      !partitions->entries[old_value].uses || old_value == new_value) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_value_fact_vector_partitions_ensure_capacity(table, new_value));
  loom_value_fact_vector_partition_use_t* use =
      partitions->entries[old_value].uses;
  partitions->entries[old_value].uses = NULL;
  while (use) {
    loom_value_fact_vector_partition_use_t* next = use->next;
    if (use->storage->available_views &
        loom_value_fact_vector_partition_use_views(use)) {
      loom_value_fact_vector_partition_component_t* components =
          (loom_value_fact_vector_partition_component_t*)(use->storage + 1);
      components[use->component_index].value = new_value;
      use->next = partitions->entries[new_value].uses;
      partitions->entries[new_value].uses = use;
    }
    use = next;
  }
  return iree_ok_status();
}

void loom_value_fact_table_invalidate_vector_partition_value(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  loom_value_fact_vector_partitions_t* partitions = table->vector_partitions;
  if (!partitions || value_id >= partitions->capacity) {
    return;
  }
  for (loom_value_fact_vector_partition_use_t* use =
           partitions->entries[value_id].uses;
       use; use = use->next) {
    use->storage->available_views &=
        ~loom_value_fact_vector_partition_use_views(use);
  }
  partitions->entries[value_id] = (loom_value_fact_vector_partition_entry_t){0};
}

iree_status_t loom_value_fact_table_vector_partition_component_scratch(
    loom_value_fact_table_t* table, iree_host_size_t count,
    loom_value_fact_vector_partition_component_t** out_components) {
  if (count <= table->scratch.vector_partition_components.capacity) {
    *out_components = table->scratch.vector_partition_components.values;
    return iree_ok_status();
  }
  loom_value_fact_vector_partition_component_t* new_scratch = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(table->transient_arena, count,
                                                 sizeof(*new_scratch),
                                                 (void**)&new_scratch));
  table->scratch.vector_partition_components.values = new_scratch;
  table->scratch.vector_partition_components.capacity = count;
  *out_components = new_scratch;
  return iree_ok_status();
}

bool loom_value_fact_table_clear_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  loom_value_fact_vector_partitions_t* partitions = table->vector_partitions;
  if (partitions && value_id < partitions->capacity &&
      partitions->entries[value_id].storage) {
    partitions->entries[value_id].storage = NULL;
    return true;
  }
  return false;
}

static bool loom_value_fact_vector_partition_components_equal(
    const loom_value_fact_vector_partition_component_t* lhs,
    const loom_value_fact_vector_partition_component_t* rhs, uint32_t count) {
  return count == 0 || memcmp(lhs, rhs, count * sizeof(*lhs)) == 0;
}

iree_status_t loom_value_fact_table_define_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_vector_partition_t partition, bool* inout_changed) {
  if (partition.count == 0 && partition.snapshot_count == 0) {
    if (loom_value_fact_table_clear_vector_partition(table, value_id) &&
        inout_changed) {
      *inout_changed = true;
    }
    return iree_ok_status();
  }
  IREE_ASSERT(partition.count == 0 || partition.components != NULL);
  IREE_ASSERT_NE(partition.lane_count, 0u);
  loom_value_id_t maximum_value_id = value_id;
  uint64_t lane_count = 0;
  for (uint32_t i = 0; i < partition.count; ++i) {
    IREE_ASSERT_NE(partition.components[i].lane_count, 0u);
    lane_count += partition.components[i].lane_count;
    maximum_value_id =
        iree_max(maximum_value_id, partition.components[i].value);
    if (partition.regular_lane_count != 0) {
      IREE_ASSERT_LE(partition.components[i].lane_count,
                     partition.regular_lane_count);
      if (i + 1u < partition.count) {
        IREE_ASSERT_EQ(partition.components[i].lane_count,
                       partition.regular_lane_count);
      }
    }
  }
  IREE_ASSERT(partition.count == 0 || lane_count == partition.lane_count);
  uint64_t snapshot_lane_count = 0;
  if (partition.snapshot_count == 0) {
    IREE_ASSERT(partition.snapshot_components == NULL);
    IREE_ASSERT_EQ(partition.snapshot_lane_count, 0u);
  } else {
    IREE_ASSERT(partition.snapshot_components != NULL);
    IREE_ASSERT_NE(partition.snapshot_lane_count, 0u);
    for (uint32_t i = 0; i < partition.snapshot_count; ++i) {
      IREE_ASSERT_NE(partition.snapshot_components[i].lane_count, 0u);
      IREE_ASSERT_LE(partition.snapshot_components[i].lane_count,
                     partition.snapshot_lane_count);
      if (i + 1u < partition.snapshot_count) {
        IREE_ASSERT_EQ(partition.snapshot_components[i].lane_count,
                       partition.snapshot_lane_count);
      }
      snapshot_lane_count += partition.snapshot_components[i].lane_count;
      maximum_value_id =
          iree_max(maximum_value_id, partition.snapshot_components[i].value);
    }
    IREE_ASSERT_EQ(snapshot_lane_count, partition.lane_count);
  }
  const loom_value_fact_vector_partition_t previous =
      loom_value_fact_table_query_vector_partition(table, value_id);
  const iree_host_size_t component_bytes =
      partition.count * sizeof(*partition.components);
  const bool snapshot_reuses_components =
      partition.snapshot_count == partition.count &&
      loom_value_fact_vector_partition_components_equal(
          partition.snapshot_components, partition.components, partition.count);
  const iree_host_size_t snapshot_component_bytes =
      snapshot_reuses_components
          ? 0
          : partition.snapshot_count * sizeof(*partition.snapshot_components);
  if (previous.count == partition.count &&
      previous.lane_count == partition.lane_count &&
      previous.regular_lane_count == partition.regular_lane_count &&
      previous.snapshot_count == partition.snapshot_count &&
      previous.snapshot_lane_count == partition.snapshot_lane_count &&
      loom_value_fact_vector_partition_components_equal(
          previous.components, partition.components, partition.count) &&
      loom_value_fact_vector_partition_components_equal(
          previous.snapshot_components, partition.snapshot_components,
          partition.snapshot_count)) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_value_fact_vector_partitions_ensure_capacity(
      table, maximum_value_id));
  const iree_host_size_t component_count =
      (iree_host_size_t)partition.count +
      (snapshot_reuses_components ? 0 : partition.snapshot_count);
  loom_value_fact_vector_partition_storage_t* storage = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(
      table->transient_arena,
      sizeof(*storage) + component_bytes + snapshot_component_bytes +
          component_count * sizeof(loom_value_fact_vector_partition_use_t),
      (void**)&storage));
  loom_value_fact_vector_partition_component_t* components =
      (loom_value_fact_vector_partition_component_t*)(storage + 1);
  if (component_bytes) {
    memcpy(components, partition.components, component_bytes);
  }
  loom_value_fact_vector_partition_component_t* snapshot_components = NULL;
  if (partition.snapshot_count != 0) {
    if (snapshot_reuses_components) {
      snapshot_components = components;
    } else {
      snapshot_components = components + partition.count;
      memcpy(snapshot_components, partition.snapshot_components,
             snapshot_component_bytes);
    }
  }
  storage->partition = (loom_value_fact_vector_partition_t){
      .components = partition.count ? components : NULL,
      .snapshot_components = snapshot_components,
      .count = partition.count,
      .lane_count = partition.lane_count,
      .regular_lane_count = partition.regular_lane_count,
      .snapshot_count = partition.snapshot_count,
      .snapshot_lane_count = partition.snapshot_lane_count,
  };
  storage->available_views =
      (partition.count ? LOOM_VECTOR_PARTITION_EXACT : 0) |
      (partition.snapshot_count ? LOOM_VECTOR_PARTITION_SNAPSHOT : 0);
  loom_value_fact_vector_partition_use_t* uses =
      (loom_value_fact_vector_partition_use_t*)(components + component_count);
  for (iree_host_size_t i = 0; i < component_count; ++i) {
    loom_value_fact_vector_partition_entry_t* entry =
        &table->vector_partitions->entries[components[i].value];
    uses[i] = (loom_value_fact_vector_partition_use_t){
        .next = entry->uses,
        .storage = storage,
        .component_index = i,
    };
    entry->uses = &uses[i];
  }
  table->vector_partitions->entries[value_id].storage = storage;
  if (inout_changed) {
    *inout_changed = true;
  }
  return iree_ok_status();
}

iree_status_t loom_value_fact_table_forward_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t source_value_id,
    loom_value_id_t result_value_id, bool* inout_changed) {
  loom_value_fact_vector_partitions_t* partitions = table->vector_partitions;
  if (!partitions || source_value_id >= partitions->capacity ||
      !partitions->entries[source_value_id].storage ||
      !partitions->entries[source_value_id].storage->available_views) {
    if (loom_value_fact_table_clear_vector_partition(table, result_value_id) &&
        inout_changed) {
      *inout_changed = true;
    }
    return iree_ok_status();
  }
  loom_value_fact_vector_partition_storage_t* storage =
      partitions->entries[source_value_id].storage;
  if (result_value_id < partitions->capacity &&
      partitions->entries[result_value_id].storage == storage) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_value_fact_vector_partitions_ensure_capacity(
      table, result_value_id));
  table->vector_partitions->entries[result_value_id].storage = storage;
  if (inout_changed) {
    *inout_changed = true;
  }
  return iree_ok_status();
}
