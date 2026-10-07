// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Analysis observations for target-independent authored program tests.

#ifndef LOOM_TOOLS_LOOM_CHECK_ANALYSIS_H_
#define LOOM_TOOLS_LOOM_CHECK_ANALYSIS_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "iree/base/string_builder.h"
#include "loom/ops/op_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Appends liveness JSON for a verified function body. Analysis scratch belongs
// to |arena|; the module remains unchanged.
iree_status_t loom_check_emit_liveness(loom_module_t* module,
                                       loom_func_like_t function,
                                       iree_arena_allocator_t* arena,
                                       iree_string_builder_t* output);

// Appends allocation instance-lifetime and pairwise workgroup interference
// proofs through the same queries consumed by static storage lowering/packing.
// Analysis scratch belongs to |arena|; the module remains unchanged.
iree_status_t loom_check_emit_storage_interference(
    loom_module_t* module, loom_func_like_t function,
    iree_arena_allocator_t* arena, iree_string_builder_t* output);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_LOOM_CHECK_ANALYSIS_H_
