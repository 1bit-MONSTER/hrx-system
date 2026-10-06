// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/io/source.h"

#include <string.h>

#include "loom/error/source.h"

void loom_tooling_source_storage_initialize(
    iree_arena_block_pool_t* block_pool,
    loom_tooling_source_storage_t* out_storage) {
  *out_storage = (loom_tooling_source_storage_t){0};
  iree_arena_initialize(block_pool, &out_storage->arena);
}

void loom_tooling_source_storage_deinitialize(
    loom_tooling_source_storage_t* storage) {
  iree_arena_deinitialize(&storage->arena);
  *storage = (loom_tooling_source_storage_t){0};
}

static iree_status_t loom_tooling_source_copy(iree_arena_allocator_t* arena,
                                              iree_string_view_t value,
                                              iree_string_view_t* out_value) {
  char* data = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, value.size + 1, (void**)&data));
  if (value.size) {
    memcpy(data, value.data, value.size);
  }
  data[value.size] = 0;
  *out_value = iree_make_string_view(data, value.size);
  return iree_ok_status();
}

iree_status_t loom_tooling_source_storage_insert(
    loom_tooling_source_storage_t* storage, loom_source_id_t source_id,
    iree_string_view_t filename, iree_string_view_t source) {
  if (source_id == LOOM_SOURCE_ID_INVALID) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "cannot capture an invalid source ID");
  }
  iree_host_size_t required_count = (iree_host_size_t)source_id + 1;
  if (required_count > storage->capacity) {
    iree_host_size_t capacity =
        iree_max(required_count, iree_max(storage->capacity * 2, 4));
    loom_source_entry_t* entries = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        &storage->arena, capacity, sizeof(*entries), (void**)&entries));
    for (iree_host_size_t i = 0; i < capacity; ++i) {
      entries[i] = (loom_source_entry_t){.source_id = LOOM_SOURCE_ID_INVALID};
    }
    if (storage->table.count) {
      memcpy(entries, storage->table.entries,
             storage->table.count * sizeof(*entries));
    }
    storage->table.entries = entries;
    storage->capacity = capacity;
  }
  loom_source_entry_t* entry =
      (loom_source_entry_t*)&storage->table.entries[source_id];
  if (entry->source_id != LOOM_SOURCE_ID_INVALID) {
    if (!iree_string_view_equal(entry->filename, filename) ||
        !iree_string_view_equal(entry->source, source)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "conflicting source snapshots for '%.*s'",
                              (int)filename.size, filename.data);
    }
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_tooling_source_copy(&storage->arena, filename, &entry->filename));
  IREE_RETURN_IF_ERROR(
      loom_tooling_source_copy(&storage->arena, source, &entry->source));
  entry->source_id = source_id;
  storage->table.count = iree_max(storage->table.count, required_count);
  return iree_ok_status();
}

iree_status_t loom_tooling_source_storage_project(
    loom_tooling_source_storage_t* storage, const loom_module_t* target_module,
    const loom_source_table_resolver_t* source_table,
    const loom_source_id_t* target_sources) {
  storage->table.module = target_module;
  for (iree_host_size_t i = 0; i < source_table->count; ++i) {
    const loom_source_entry_t* entry = &source_table->entries[i];
    if (entry->source_id == LOOM_SOURCE_ID_INVALID) {
      continue;
    }
    loom_source_id_t target_id = target_sources[entry->source_id];
    if (target_id == LOOM_SOURCE_ID_INVALID) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_tooling_source_storage_insert(
        storage, target_id, entry->filename, entry->source));
  }
  return iree_ok_status();
}

loom_source_resolver_t loom_tooling_source_storage_resolver(
    const loom_tooling_source_storage_t* storage) {
  return (loom_source_resolver_t){
      .fn = loom_source_table_resolve,
      .user_data = (void*)&storage->table,
  };
}
