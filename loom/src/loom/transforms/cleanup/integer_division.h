// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Integer division combination over canonical source IR.

#ifndef LOOM_TRANSFORMS_CLEANUP_INTEGER_DIVISION_H_
#define LOOM_TRANSFORMS_CLEANUP_INTEGER_DIVISION_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_integer_division_combine_result_t {
  // Number of combined quotient/remainder operations created.
  uint32_t combined_operation_count;
  // Number of standalone quotient/remainder operations retired.
  uint32_t retired_operation_count;
} loom_integer_division_combine_result_t;

// Plans and applies quotient/remainder combination across |function|.
//
// |facts| must describe the immutable canonical function. Planning consumes
// retained value identities and exact divisor facts directly, and reuses the
// dominance snapshot constructed by its ordered walk. The complete plan is
// verified before any IR mutation. After this returns a changed result,
// |facts| no longer describe the function and must be invalidated by the pass
// owner.
iree_status_t loom_integer_division_combine(
    loom_module_t* module, loom_func_like_t function,
    const loom_value_fact_table_t* facts, iree_arena_allocator_t* parent_arena,
    loom_integer_division_combine_result_t* out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_CLEANUP_INTEGER_DIVISION_H_
