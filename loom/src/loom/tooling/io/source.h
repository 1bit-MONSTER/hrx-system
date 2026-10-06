// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_IO_SOURCE_H_
#define LOOM_TOOLING_IO_SOURCE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/error/source.h"

#ifdef __cplusplus
extern "C" {
#endif

// Immutable source snapshots owned independently of frontend and input-module
// lifetimes. Keep this object at a stable address while using its resolver.
typedef struct loom_tooling_source_storage_t {
  // Source-ID-indexed entries. Missing snapshots have an invalid source ID.
  loom_source_table_resolver_t table;
  // Owns the table, filenames, and source bytes.
  iree_arena_allocator_t arena;
  // Allocated entries in the table.
  iree_host_size_t capacity;
} loom_tooling_source_storage_t;

void loom_tooling_source_storage_initialize(
    iree_arena_block_pool_t* block_pool,
    loom_tooling_source_storage_t* out_storage);
void loom_tooling_source_storage_deinitialize(
    loom_tooling_source_storage_t* storage);

// Copies a source snapshot into |storage| at a module-owned source ID.
// Repeated admission of the same identity requires identical name and bytes.
iree_status_t loom_tooling_source_storage_insert(
    loom_tooling_source_storage_t* storage, loom_source_id_t source_id,
    iree_string_view_t filename, iree_string_view_t source);

// Copies captured snapshots through a producer-owned source correspondence.
// |target_sources| is indexed by input source ID. Inputs with no snapshot need
// no entry in |source_table|; invalid target IDs represent omitted sources.
// All projections into |storage| belong to the same |target_module|.
iree_status_t loom_tooling_source_storage_project(
    loom_tooling_source_storage_t* storage, const loom_module_t* target_module,
    const loom_source_table_resolver_t* source_table,
    const loom_source_id_t* target_sources);

loom_source_resolver_t loom_tooling_source_storage_resolver(
    const loom_tooling_source_storage_t* storage);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_IO_SOURCE_H_
