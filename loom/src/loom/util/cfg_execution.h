// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Execution-frequency uncertainty over retained CFG control dependence.

#ifndef LOOM_UTIL_CFG_EXECUTION_H_
#define LOOM_UTIL_CFG_EXECUTION_H_

#include "iree/base/bitmap.h"
#include "loom/util/cfg_control.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns true when |block_index| selects a control alternative whose
// execution frequency is modeled independently by the caller.
typedef bool (*loom_cfg_execution_selector_is_modeled_fn_t)(
    const void* user_data, uint16_t block_index);

typedef struct loom_cfg_execution_selector_model_t {
  // Selector classification callback, or NULL when no selector is modeled.
  loom_cfg_execution_selector_is_modeled_fn_t is_modeled;
  // Caller-owned payload passed to |is_modeled|.
  const void* user_data;
} loom_cfg_execution_selector_model_t;

// Classifies blocks whose execution count depends on an unmodeled selector.
//
// Control alternatives affect the path from their target up to, but excluding,
// the selector's immediate postdominator. Consequently a reconverged block is
// exact again when its enclosing execution count is exact. Modeled selectors,
// such as a fixed-trip loop header, do not introduce uncertainty; their caller
// accounts for the corresponding multiplicity separately.
//
// The returned bitmap has one bit per CFG block and is owned by |arena|.
// Unreachable blocks are unmarked. Classification consumes the retained
// compressed control graph in O(B+C+I) time for B blocks, C control components,
// and I component inputs. O(C) temporary storage is released before return.
iree_status_t loom_cfg_execution_classify_unmodeled_blocks(
    const loom_cfg_control_t* control,
    loom_cfg_execution_selector_model_t selector_model,
    iree_arena_allocator_t* arena, iree_bitmap_t* out_unmodeled_blocks);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_UTIL_CFG_EXECUTION_H_
