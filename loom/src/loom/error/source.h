// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Borrowed source resolution for diagnostics and source-aware compilation.

#ifndef LOOM_ERROR_SOURCE_H_
#define LOOM_ERROR_SOURCE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/error/diagnostic.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resolves a module-local location to a borrowed source range. Returns false
// when the location cannot be resolved; |out_range| is only valid on success.
// The resolver owner keeps the returned filename and source bytes alive for
// the duration of their use. Consumers retaining source data must copy it.
typedef bool (*loom_source_resolver_fn_t)(void* user_data,
                                          const loom_module_t* module,
                                          loom_location_id_t location,
                                          loom_source_range_t* out_range);

typedef struct loom_source_resolver_t {
  // Optional enrichment callback. Without it, module locations still resolve.
  loom_source_resolver_fn_t fn;
  // Borrowed callback context, live through the final resolution call.
  void* user_data;
} loom_source_resolver_t;

// Resolves |location| using the callback when available, otherwise retaining
// the module's recorded filename and coordinates with UNAVAILABLE_SOURCE
// provenance. Success identifies a location, not the availability of text.
// Tagged locations retain their child's identity; locations without a single
// file origin return false. Callback ranges borrow the resolver owner's
// storage; metadata-only ranges borrow |module|. Retaining consumers must copy
// both.
bool loom_source_resolve(loom_source_resolver_t resolver,
                         const loom_module_t* module,
                         loom_location_id_t location,
                         loom_source_range_t* out_range);

// Source bytes associated with a source identity in the module being resolved.
typedef struct loom_source_entry_t {
  // Module-local source identity, or LOOM_SOURCE_ID_INVALID for an empty entry.
  loom_source_id_t source_id;
  // Borrowed original source bytes.
  iree_string_view_t source;
  // Borrowed source filename.
  iree_string_view_t filename;
} loom_source_entry_t;

// Borrowed source entries for loom_source_table_resolve. Entries need not be
// dense or ordered by source ID. Linking projects IDs into the target module
// before its locations are resolved against this table.
typedef struct loom_source_table_resolver_t {
  // Borrowed module whose source IDs identify these exact snapshots.
  const loom_module_t* module;
  // Borrowed entries and their strings, live through the final resolution use.
  const loom_source_entry_t* entries;
  // Number of entries, including any empty entries.
  iree_host_size_t count;
} loom_source_table_resolver_t;

// Borrowed snapshots following module replacements during one compilation.
typedef struct loom_source_table_projection_t {
  // Current module's entries; filenames and text remain borrowed from the
  // input.
  loom_source_table_resolver_t table;
  // Owns projected entry arrays through the final resolution call.
  iree_arena_allocator_t* arena;
} loom_source_table_projection_t;

// Linker source callback for a loom_source_table_projection_t. Projects entries
// through the producer's source-ID map without copying their filenames or text.
// Inputs without snapshots allocate nothing. The output table is indexed by
// target source ID, with invalid IDs for missing snapshots.
iree_status_t loom_source_table_project(void* user_data,
                                        const loom_module_t* source_module,
                                        const loom_module_t* target_module,
                                        const loom_source_id_t* target_sources);

// Resolves file locations against a loom_source_table_resolver_t passed as
// |user_data|. Other modules, unknown, non-file, and missing sources return
// false, as do unavailable, reversed, or out-of-snapshot coordinates. Success
// denotes an exact original range; coordinates are never clamped into different
// spelling.
bool loom_source_table_resolve(void* user_data, const loom_module_t* module,
                               loom_location_id_t location,
                               loom_source_range_t* out_range);

// Computes the byte offset of a one-based line and Unicode code-point column.
// Line zero resolves to offset zero. A column at or before one resolves to the
// line's start; a column past the line resolves to its end. A line past the
// source resolves to source.size. This matches Loom text tokenizer coordinates.
iree_host_size_t loom_source_byte_offset(iree_string_view_t source,
                                         uint32_t line, uint32_t column);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ERROR_SOURCE_H_
