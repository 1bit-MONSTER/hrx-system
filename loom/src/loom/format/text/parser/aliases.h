// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_FORMAT_TEXT_PARSER_ALIASES_H_
#define LOOM_FORMAT_TEXT_PARSER_ALIASES_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Source names may outnumber canonical encodings and survive independently of
// their display aliases. A zero-initialized table allocates lazily on
// insertion.
typedef struct loom_alias_table_t {
  // Parser-arena buckets mapping interned source names to 1-based encoding IDs.
  loom_intern_table_t index;
} loom_alias_table_t;

// Registers a known-unique, module-interned source name for an existing
// encoding. Bucket growth reuses old segments; the parser arena owns their
// lifetime.
iree_status_t loom_alias_table_add(loom_alias_table_t* table,
                                   iree_arena_allocator_t* arena,
                                   loom_string_id_t name_id,
                                   uint16_t encoding_id);

// Returns the encoding ID for a bare source |name|, or 0 if not found.
// Lookup does not intern unknown names or allocate storage.
uint16_t loom_alias_table_lookup(const loom_alias_table_t* table,
                                 const loom_module_t* module,
                                 iree_string_view_t name);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_FORMAT_TEXT_PARSER_ALIASES_H_
