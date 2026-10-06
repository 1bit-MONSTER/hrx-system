// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/error/source.h"

#include <string.h>

#include "iree/base/internal/unicode.h"
#include "loom/ir/module.h"

// Tagged locations preserve a single origin. Fused locations have no designated
// primary file and cannot be represented by one source range.
static const loom_location_entry_t* loom_source_file_location(
    const loom_module_t* module, loom_location_id_t location) {
  if (!module) {
    return NULL;
  }
  while (location != LOOM_LOCATION_UNKNOWN) {
    const loom_location_entry_t* entry =
        loom_location_table_const_entry(&module->locations, location);
    if (entry->kind == LOOM_LOCATION_FILE) {
      return entry;
    }
    if (entry->kind != LOOM_LOCATION_TAGGED) {
      return NULL;
    }
    location = entry->tagged.child;
  }
  return NULL;
}

bool loom_source_resolve(loom_source_resolver_t resolver,
                         const loom_module_t* module,
                         loom_location_id_t location,
                         loom_source_range_t* out_range) {
  if (resolver.fn &&
      resolver.fn(resolver.user_data, module, location, out_range)) {
    return true;
  }
  const loom_location_entry_t* entry =
      loom_source_file_location(module, location);
  if (!entry) {
    return false;
  }
  *out_range = (loom_source_range_t){
      .provenance = LOOM_SOURCE_PROVENANCE_UNAVAILABLE_SOURCE,
      .filename = module->sources.entries[entry->file.source_id],
      .start_line = entry->file.start_line,
      .start_column = entry->file.start_col,
      .end_line = entry->file.end_line,
      .end_column = entry->file.end_col,
  };
  return true;
}

// Returns an exact position when present, while always publishing the clamped
// byte offset used by source highlighting.
static bool loom_source_find_position(iree_string_view_t source, uint32_t line,
                                      uint32_t column,
                                      iree_host_size_t* out_offset) {
  if (line == 0) {
    *out_offset = 0;
    return false;
  }
  // Scan newlines to find the byte offset of the start of |line|.
  uint32_t current_line = 1;
  iree_host_size_t offset = 0;
  while (current_line < line && offset < source.size) {
    if (source.data[offset] == '\n') {
      ++current_line;
    }
    ++offset;
  }
  if (current_line < line) {
    *out_offset = source.size;
    return false;
  }
  // Walk UTF-8 codepoints to reach the target column (1-based).
  // Column 1 means "start of line" = offset stays where it is.
  uint32_t current_column = 1;
  while (current_column < column && offset < source.size &&
         source.data[offset] != '\n') {
    iree_unicode_utf8_decode(source, &offset);
    ++current_column;
  }
  *out_offset = iree_min(offset, source.size);
  return offset <= source.size && current_column == column;
}

iree_host_size_t loom_source_byte_offset(iree_string_view_t source,
                                         uint32_t line, uint32_t column) {
  iree_host_size_t offset;
  loom_source_find_position(source, line, column, &offset);
  return offset;
}

iree_status_t loom_source_table_project(
    void* user_data, const loom_module_t* source_module,
    const loom_module_t* target_module,
    const loom_source_id_t* target_sources) {
  loom_source_table_projection_t* projection = user_data;
  const loom_source_table_resolver_t* table = &projection->table;
  iree_host_size_t count = table->count ? target_module->sources.count : 0;
  loom_source_entry_t* entries = NULL;
  if (count) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        projection->arena, count, sizeof(*entries), (void**)&entries));
    for (iree_host_size_t i = 0; i < count; ++i) {
      entries[i] = (loom_source_entry_t){.source_id = LOOM_SOURCE_ID_INVALID};
    }
    for (iree_host_size_t i = 0; i < table->count; ++i) {
      const loom_source_entry_t* entry = &table->entries[i];
      if (entry->source_id == LOOM_SOURCE_ID_INVALID) {
        continue;
      }
      loom_source_id_t target_id = target_sources[entry->source_id];
      if (target_id == LOOM_SOURCE_ID_INVALID) {
        continue;
      }
      entries[target_id] = *entry;
      entries[target_id].source_id = target_id;
    }
  }
  projection->table = (loom_source_table_resolver_t){
      .module = target_module, .entries = entries, .count = count};
  return iree_ok_status();
}

void loom_source_storage_initialize(iree_allocator_t allocator,
                                    loom_source_storage_t* out_storage) {
  *out_storage = (loom_source_storage_t){.allocator = allocator};
}

void loom_source_storage_deinitialize(loom_source_storage_t* storage) {
  for (iree_host_size_t i = 0; i < storage->table.count; ++i) {
    const loom_source_entry_t* entry = &storage->table.entries[i];
    if (entry->source_id != LOOM_SOURCE_ID_INVALID) {
      iree_allocator_free(storage->allocator, (void*)entry->filename.data);
    }
  }
  iree_allocator_free(storage->allocator, (void*)storage->table.entries);
  *storage = (loom_source_storage_t){0};
}

static iree_status_t loom_source_storage_allocate_entry(
    loom_source_storage_t* storage, loom_source_id_t source_id,
    iree_string_view_t filename, iree_string_view_t source,
    loom_source_entry_t* out_entry) {
  iree_host_size_t filename_capacity = 0;
  iree_host_size_t allocation_size = 0;
  if (!iree_host_size_checked_add(filename.size, 1, &filename_capacity) ||
      !iree_host_size_checked_add(filename_capacity, source.size,
                                  &allocation_size) ||
      !iree_host_size_checked_add(allocation_size, 1, &allocation_size)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "source snapshot allocation size overflow");
  }
  char* storage_ptr = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc_uninitialized(
      storage->allocator, allocation_size, (void**)&storage_ptr));
  if (filename.size) {
    memcpy(storage_ptr, filename.data, filename.size);
  }
  storage_ptr[filename.size] = 0;
  char* source_ptr = storage_ptr + filename_capacity;
  if (source.size) {
    memcpy(source_ptr, source.data, source.size);
  }
  source_ptr[source.size] = 0;
  *out_entry = (loom_source_entry_t){
      .source_id = source_id,
      .source = iree_make_string_view(source_ptr, source.size),
      .filename = iree_make_string_view(storage_ptr, filename.size),
  };
  return iree_ok_status();
}

iree_status_t loom_source_storage_insert(loom_source_storage_t* storage,
                                         loom_source_id_t source_id,
                                         iree_string_view_t filename,
                                         iree_string_view_t source) {
  if (source_id == LOOM_SOURCE_ID_INVALID) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "cannot capture an invalid source ID");
  }
  iree_host_size_t required_count = (iree_host_size_t)source_id + 1;
  if (required_count > storage->capacity) {
    const iree_host_size_t old_capacity = storage->capacity;
    loom_source_entry_t* entries = (loom_source_entry_t*)storage->table.entries;
    IREE_RETURN_IF_ERROR(iree_allocator_grow_array(
        storage->allocator, required_count, sizeof(*entries),
        &storage->capacity, (void**)&entries));
    for (iree_host_size_t i = old_capacity; i < storage->capacity; ++i) {
      entries[i] = (loom_source_entry_t){.source_id = LOOM_SOURCE_ID_INVALID};
    }
    storage->table.entries = entries;
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
  IREE_RETURN_IF_ERROR(loom_source_storage_allocate_entry(
      storage, source_id, filename, source, entry));
  storage->table.count = iree_max(storage->table.count, required_count);
  return iree_ok_status();
}

iree_status_t loom_source_storage_project(
    void* user_data, const loom_module_t* source_module,
    const loom_module_t* target_module,
    const loom_source_id_t* target_sources) {
  loom_source_storage_projection_t* projection = user_data;
  loom_source_storage_t* storage = projection->target;
  const loom_source_table_resolver_t* source_table = projection->source;
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
    IREE_RETURN_IF_ERROR(loom_source_storage_insert(
        storage, target_id, entry->filename, entry->source));
  }
  return iree_ok_status();
}

loom_source_resolver_t loom_source_storage_resolver(
    const loom_source_storage_t* storage) {
  return (loom_source_resolver_t){
      .fn = loom_source_table_resolve,
      .user_data = (void*)&storage->table,
  };
}

bool loom_source_table_resolve(void* user_data, const loom_module_t* module,
                               loom_location_id_t location,
                               loom_source_range_t* out_range) {
  const loom_source_table_resolver_t* table =
      (const loom_source_table_resolver_t*)user_data;
  if (!table || table->module != module || table->count == 0) {
    return false;
  }
  const loom_location_entry_t* entry =
      loom_source_file_location(module, location);
  if (!entry) {
    return false;
  }

  // Owned tables are indexed by source ID. Preserve the general resolver
  // contract for callers that provide unordered tables.
  const loom_source_entry_t* source_entry = NULL;
  if (entry->file.source_id < table->count &&
      table->entries[entry->file.source_id].source_id ==
          entry->file.source_id) {
    source_entry = &table->entries[entry->file.source_id];
  } else {
    for (iree_host_size_t i = 0; i < table->count; ++i) {
      if (table->entries[i].source_id == entry->file.source_id) {
        source_entry = &table->entries[i];
        break;
      }
    }
  }
  if (!source_entry) {
    return false;
  }

  // Only an ordered range actually present in the snapshot has exact spelling.
  // Explicit debug locations can name unavailable or out-of-snapshot positions.
  iree_host_size_t start_offset = 0, end_offset = 0;
  if (!loom_source_find_position(source_entry->source, entry->file.start_line,
                                 entry->file.start_col, &start_offset) ||
      !loom_source_find_position(source_entry->source, entry->file.end_line,
                                 entry->file.end_col, &end_offset) ||
      end_offset < start_offset) {
    return false;
  }

  *out_range = (loom_source_range_t){
      .provenance = LOOM_SOURCE_PROVENANCE_EXACT_SOURCE,
      .filename = source_entry->filename,
      .source = source_entry->source,
      .start = start_offset,
      .end = end_offset,
      .start_line = entry->file.start_line,
      .start_column = entry->file.start_col,
      .end_line = entry->file.end_line,
      .end_column = entry->file.end_col,
  };
  return true;
}
