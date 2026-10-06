// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/error/source.h"

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

  // Find the matching source buffer by source_id.
  const loom_source_entry_t* source_entry = NULL;
  for (iree_host_size_t i = 0; i < table->count; ++i) {
    if (table->entries[i].source_id == entry->file.source_id) {
      source_entry = &table->entries[i];
      break;
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
