// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/view/combine_patterns.h"

#include "loom/ops/view/ops.h"
#include "loom/transforms/cleanup/patterns.h"
#include "loom/transforms/view/atomic.h"
#include "loom/transforms/view/load_coalescing.h"
#include "loom/util/fact_extensions.h"

static iree_status_t loom_view_atomic_private_pattern(
    const loom_rewrite_pattern_t* pattern, void* context, loom_op_t* op,
    loom_rewriter_t* rewriter, bool* out_changed) {
  (void)pattern;
  (void)context;
  *out_changed = false;
  // Cleanup has no target arithmetic-mode proof for an explicit noftz update.
  if (!rewriter->fact_table ||
      iree_any_bit_set(op->instance_flags, LOOM_MEMORY_ACCESS_FLAG_NOFTZ)) {
    return iree_ok_status();
  }
  const loom_memory_access_t access =
      loom_memory_access_cast(rewriter->module, op);
  loom_value_fact_view_reference_t reference = {0};
  if (!loom_value_facts_query_view_reference(
          &rewriter->fact_table->context,
          loom_rewriter_value_facts(rewriter, loom_memory_access_view(access)),
          &reference) ||
      reference.memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_PRIVATE) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_view_atomic_rewrite_private(rewriter, op));
  *out_changed = true;
  return iree_ok_status();
}

static iree_status_t loom_view_load_coalescing_pattern(
    const loom_rewrite_pattern_t* pattern, void* context, loom_op_t* op,
    loom_rewriter_t* rewriter, bool* out_changed) {
  (void)pattern;
  loom_cleanup_pattern_context_t* cleanup_context =
      (loom_cleanup_pattern_context_t*)context;
  return loom_view_load_coalescing_rewrite(
      rewriter, cleanup_context->symbolic_expression_context, op, out_changed);
}

static const loom_rewrite_pattern_t kViewSourceCombinePatterns[] = {
    {.root_kind = LOOM_OP_VIEW_ATOMIC_LOAD,
     .match_and_rewrite = loom_view_atomic_private_pattern},
    {.root_kind = LOOM_OP_VIEW_ATOMIC_STORE,
     .match_and_rewrite = loom_view_atomic_private_pattern},
    {.root_kind = LOOM_OP_VIEW_ATOMIC_REDUCE,
     .match_and_rewrite = loom_view_atomic_private_pattern},
    {.root_kind = LOOM_OP_VIEW_ATOMIC_RMW,
     .match_and_rewrite = loom_view_atomic_private_pattern},
    {.root_kind = LOOM_OP_VIEW_ATOMIC_CMPXCHG,
     .match_and_rewrite = loom_view_atomic_private_pattern},
    {
        .root_kind = LOOM_OP_VIEW_LOAD,
        .match_and_rewrite = loom_view_load_coalescing_pattern,
    },
};

const loom_rewrite_pattern_provider_t
    loom_view_source_combine_pattern_provider = {
        .name = IREE_SVL("view"),
        .patterns = kViewSourceCombinePatterns,
        .pattern_count = IREE_ARRAYSIZE(kViewSourceCombinePatterns),
};
