// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_IMPORT_H_
#define LOOM_IMPORT_CXX_IMPORT_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/import/cxx/source/options.h"
#include "loom/import/cxx/source/preamble.h"
#include "loom/ir/module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Imports one C/C++ translation unit into an ordinary verified Loom module.
// The finalized context must contain the core dialects. Context and block pool
// outlive the returned module; all source/AST storage can be released on
// return. No cleanup or optimization passes run here. On success the caller
// owns *out_module and frees it with loom_module_free(). Source rejection emits
// diagnostics and returns OK with *out_module=NULL. Infrastructure and option
// failures return a non-OK status with *out_module=NULL.
iree_status_t loom_cxx_import(iree_string_view_t source,
                              iree_string_view_t filename,
                              loom_context_t* context,
                              iree_arena_block_pool_t* block_pool,
                              const loom_cxx_import_options_t* options,
                              iree_allocator_t host_allocator,
                              loom_module_t** out_module);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_IMPORT_CXX_IMPORT_H_
