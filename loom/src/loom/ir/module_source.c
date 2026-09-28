// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/module_source.h"

#include <string.h>

#include "loom/ir/structural_hash.h"

static uint32_t loom_source_name_hash(iree_string_view_t name) {
  return loom_structural_hash_finalize(loom_structural_hash_mix_bytes(
      loom_structural_hash_initialize(), name.data, name.size));
}

// Publishes only a fully initialized index. The canonical source rows remain
// unchanged if allocation fails, including when the index spans many blocks.
static iree_status_t loom_source_table_create_index(loom_module_t* module) {
  const iree_arena_checkpoint_t checkpoint =
      iree_arena_checkpoint_save(&module->arena);
  loom_intern_table_t* index = NULL;
  iree_status_t status =
      iree_arena_allocate(&module->arena, sizeof(*index), (void**)&index);
  if (iree_status_is_ok(status)) {
    status = loom_intern_table_initialize(
        &module->arena,
        loom_intern_table_capacity_for_entries(module->sources.count), index);
  }
  if (iree_status_is_ok(status)) {
    module->sources.name_index = index;
  } else {
    iree_arena_checkpoint_restore(&checkpoint);
  }
  return status;
}

// Trusted unique appends need no index work. The next registration incorporates
// their suffix once; index.count retains that frontier across every caller.
static iree_status_t loom_source_table_index_appended(loom_module_t* module) {
  if (module->sources.name_index == NULL) {
    IREE_RETURN_IF_ERROR(loom_source_table_create_index(module));
  }
  loom_intern_table_t* index = module->sources.name_index;
  while (index->count < module->sources.count) {
    const uint32_t source_id = (uint32_t)index->count;
    const uint32_t hash =
        loom_source_name_hash(module->sources.entries[source_id]);
    iree_host_size_t slot = loom_intern_table_find_empty_slot(index, hash);
    IREE_RETURN_IF_ERROR(loom_intern_table_reserve_insert(
        &module->arena, index, hash, /*insertion_count=*/1, &slot));
    loom_intern_table_insert(index, slot, hash, source_id);
  }
  return iree_ok_status();
}

typedef struct loom_source_name_query_t {
  // Canonical rows owned by the queried module.
  const loom_source_table_t* table;
  // Borrowed name being registered.
  iree_string_view_t name;
} loom_source_name_query_t;

static bool loom_source_name_equal(const void* context, uint32_t source_id) {
  const loom_source_name_query_t* query = context;
  return iree_string_view_equal(query->table->entries[source_id], query->name);
}

iree_status_t loom_module_append_source(loom_module_t* module,
                                        iree_string_view_t name,
                                        loom_source_id_t* out_source_id) {
  *out_source_id = LOOM_SOURCE_ID_INVALID;

  // Source IDs are 0-based uint16_t with UINT16_MAX reserved for the sentinel.
  if (module->sources.count >= LOOM_SOURCE_ID_INVALID) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "module source table full (%" PRIhsz " entries, max id %u)",
        module->sources.count, (unsigned)(LOOM_SOURCE_ID_INVALID - 1));
  }
  if (module->sources.count == module->sources.capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        &module->arena, module->sources.count, /*minimum_capacity=*/4,
        sizeof(*module->sources.entries), &module->sources.capacity,
        (void**)&module->sources.entries));
  }
  char* copy = NULL;
  if (!iree_string_view_is_empty(name)) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate(&module->arena, name.size, (void**)&copy));
    memcpy(copy, name.data, name.size);
  }
  const iree_host_size_t source_id = module->sources.count++;
  module->sources.entries[source_id] = iree_make_string_view(copy, name.size);
  *out_source_id = (loom_source_id_t)source_id;
  return iree_ok_status();
}

// Index construction and hashing stay outside the bounded small-table path,
// including its stack frame when compiling with whole-program optimization.
IREE_ATTRIBUTE_NOINLINE static iree_status_t
loom_module_register_indexed_source(loom_module_t* module,
                                    iree_string_view_t name,
                                    loom_source_id_t* out_source_id) {
  IREE_RETURN_IF_ERROR(loom_source_table_index_appended(module));
  loom_intern_table_t* index = module->sources.name_index;
  const uint32_t hash = loom_source_name_hash(name);
  const loom_source_name_query_t query = {&module->sources, name};
  const loom_intern_probe_t probe =
      loom_intern_table_probe(index, hash, loom_source_name_equal, &query);
  if (probe.index != UINT32_MAX) {
    *out_source_id = (loom_source_id_t)probe.index;
    return iree_ok_status();
  }
  iree_host_size_t slot = probe.slot;
  IREE_RETURN_IF_ERROR(loom_intern_table_reserve_insert(
      &module->arena, index, hash, /*insertion_count=*/1, &slot));
  IREE_RETURN_IF_ERROR(loom_module_append_source(module, name, out_source_id));
  loom_intern_table_insert(index, slot, hash, *out_source_id);
  return iree_ok_status();
}

iree_status_t loom_module_register_source(loom_module_t* module,
                                          iree_string_view_t name,
                                          loom_source_id_t* out_source_id) {
  *out_source_id = LOOM_SOURCE_ID_INVALID;
  // Small source sets do not amortize the index's first bucket segment.
  if (module->sources.count > 32) {
    return loom_module_register_indexed_source(module, name, out_source_id);
  }
  // Ordinary single-file modules stay allocation-free on a repeated lookup.
  for (iree_host_size_t i = 0; i < module->sources.count; ++i) {
    if (iree_string_view_equal(module->sources.entries[i], name)) {
      *out_source_id = (loom_source_id_t)i;
      return iree_ok_status();
    }
  }
  return loom_module_append_source(module, name, out_source_id);
}
