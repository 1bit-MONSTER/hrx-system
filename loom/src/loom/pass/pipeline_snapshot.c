// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/pass/pipeline_snapshot.h"

#include "loom/ir/module.h"
#include "loom/link/linker.h"
#include "loom/ops/pass/ops.h"

static iree_string_view_t loom_pass_pipeline_snapshot_normalize_symbol(
    iree_string_view_t symbol) {
  symbol = iree_string_view_trim(symbol);
  while (iree_string_view_starts_with_char(symbol, '@')) {
    symbol = iree_string_view_remove_prefix(symbol, 1);
  }
  return symbol;
}

static iree_status_t loom_pass_pipeline_snapshot_find_pipeline(
    const loom_module_t* module, iree_string_view_t symbol_name,
    const loom_op_t** out_pipeline_op) {
  *out_pipeline_op = NULL;
  const loom_string_id_t name_id =
      loom_module_lookup_string(module, symbol_name);
  const loom_symbol_id_t symbol_id =
      name_id != LOOM_STRING_ID_INVALID
          ? loom_module_find_symbol(module, name_id)
          : LOOM_SYMBOL_ID_INVALID;
  if (symbol_id == LOOM_SYMBOL_ID_INVALID) {
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "pass pipeline @%.*s was not found",
                            (int)symbol_name.size, symbol_name.data);
  }
  const loom_symbol_t* symbol = &module->symbols.entries[symbol_id];
  if (!symbol->defining_op || !loom_pass_pipeline_isa(symbol->defining_op)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "symbol @%.*s does not define a pass.pipeline",
                            (int)symbol_name.size, symbol_name.data);
  }
  *out_pipeline_op = symbol->defining_op;
  return iree_ok_status();
}

iree_status_t loom_pass_pipeline_snapshot_initialize(
    const loom_module_t* source_module, iree_string_view_t pipeline_symbol,
    iree_string_view_t identifier, iree_arena_block_pool_t* block_pool,
    iree_allocator_t allocator, loom_pass_pipeline_snapshot_t* out_snapshot) {
  *out_snapshot = (loom_pass_pipeline_snapshot_t){0};
  const iree_string_view_t symbol_name =
      loom_pass_pipeline_snapshot_normalize_symbol(pipeline_symbol);
  if (iree_string_view_is_empty(symbol_name)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "pass pipeline symbol name is required");
  }

  const loom_module_t* source_modules[] = {source_module};
  const iree_string_view_t root_symbols[] = {symbol_name};
  iree_status_t status = loom_link_materialized_modules(
      source_modules, IREE_ARRAYSIZE(source_modules),
      &(loom_link_options_t){
          .module_name = identifier,
          .root_symbols =
              {
                  .count = IREE_ARRAYSIZE(root_symbols),
                  .values = root_symbols,
              },
      },
      block_pool, allocator, &out_snapshot->module);
  if (iree_status_is_ok(status)) {
    status = loom_pass_pipeline_snapshot_find_pipeline(
        out_snapshot->module, symbol_name, &out_snapshot->pipeline_op);
  }
  if (!iree_status_is_ok(status)) {
    loom_pass_pipeline_snapshot_deinitialize(out_snapshot);
  }
  return status;
}

void loom_pass_pipeline_snapshot_deinitialize(
    loom_pass_pipeline_snapshot_t* snapshot) {
  if (snapshot == NULL) {
    return;
  }
  loom_module_free(snapshot->module);
  *snapshot = (loom_pass_pipeline_snapshot_t){0};
}
