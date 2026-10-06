// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/text/parser/aliases.h"

#include "loom/ir/module.h"

// Odd multiplication is a bijection of the complete u32 string-ID domain.
// The retained hash therefore identifies the name without a separate key row.
static uint32_t loom_alias_name_hash(loom_string_id_t name_id) {
  return name_id * 2654435769u;
}

static bool loom_alias_name_equal(const void* context, uint32_t index) {
  (void)context;
  (void)index;
  // Full-hash equality in the probe already proves equal source names.
  return true;
}

iree_status_t loom_alias_table_add(loom_alias_table_t* table,
                                   iree_arena_allocator_t* arena,
                                   loom_string_id_t name_id,
                                   uint16_t encoding_id) {
  if (table->index.capacity == 0) {
    IREE_RETURN_IF_ERROR(
        loom_intern_table_initialize(arena, 32, &table->index));
  }
  const uint32_t hash = loom_alias_name_hash(name_id);
  iree_host_size_t slot =
      loom_intern_table_find_empty_slot(&table->index, hash);
  IREE_RETURN_IF_ERROR(loom_intern_table_reserve_insert(
      arena, &table->index, hash, /*insertion_count=*/1, &slot));
  loom_intern_table_insert(&table->index, slot, hash, encoding_id);
  return iree_ok_status();
}

uint16_t loom_alias_table_lookup(const loom_alias_table_t* table,
                                 const loom_module_t* module,
                                 iree_string_view_t name) {
  if (table->index.count == 0) {
    return 0;
  }
  const loom_string_id_t name_id = loom_module_lookup_string(module, name);
  if (name_id == LOOM_STRING_ID_INVALID) {
    return 0;
  }
  const loom_intern_probe_t probe =
      loom_intern_table_probe(&table->index, loom_alias_name_hash(name_id),
                              loom_alias_name_equal, NULL);
  return probe.index != UINT32_MAX ? (uint16_t)probe.index : 0;
}
