// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_ordinal_map.h"

#include <string.h>

iree_status_t loom_value_fact_ordinal_map_initialize(
    loom_value_fact_table_t* table, const loom_value_id_t* values,
    uint16_t value_count, bool use_direct,
    loom_value_fact_ordinal_map_t* out_map) {
  *out_map = (loom_value_fact_ordinal_map_t){
      .values = values,
      .value_count = value_count,
  };
  if (!use_direct) {
    return iree_ok_status();
  }

  iree_host_size_t required_capacity = 0;
  for (uint16_t i = 0; i < value_count; ++i) {
    required_capacity =
        iree_max(required_capacity, (iree_host_size_t)values[i] + 1);
  }
  if (required_capacity > table->scratch.alias_ordinals.capacity) {
    uint16_t* new_ordinals = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        table->transient_arena, required_capacity, sizeof(*new_ordinals),
        (void**)&new_ordinals));
    memset(new_ordinals, 0, required_capacity * sizeof(*new_ordinals));
    table->scratch.alias_ordinals.values = new_ordinals;
    table->scratch.alias_ordinals.capacity = required_capacity;
  }

  uint16_t* ordinals = table->scratch.alias_ordinals.values;
  for (uint16_t i = 0; i < value_count; ++i) {
    const loom_value_id_t value_id = values[i];
    if (ordinals[value_id] == 0) {
      ordinals[value_id] = (uint16_t)(i + 1);
    }
  }
  out_map->ordinals = ordinals;
  out_map->ordinal_capacity = table->scratch.alias_ordinals.capacity;
  return iree_ok_status();
}

void loom_value_fact_ordinal_map_reset(loom_value_fact_ordinal_map_t* map) {
  if (map->ordinals) {
    for (uint16_t i = 0; i < map->value_count; ++i) {
      map->ordinals[map->values[i]] = 0;
    }
  }
  *map = (loom_value_fact_ordinal_map_t){0};
}

bool loom_value_fact_ordinal_map_find(const loom_value_fact_ordinal_map_t* map,
                                      loom_value_id_t value_id,
                                      uint16_t* out_ordinal) {
  if (map->ordinals) {
    if (value_id >= map->ordinal_capacity) {
      return false;
    }
    const uint16_t ordinal_plus_one = map->ordinals[value_id];
    if (ordinal_plus_one == 0) {
      return false;
    }
    *out_ordinal = ordinal_plus_one - 1;
    return true;
  }

  for (uint16_t i = 0; i < map->value_count; ++i) {
    if (map->values[i] == value_id) {
      *out_ordinal = i;
      return true;
    }
  }
  return false;
}
