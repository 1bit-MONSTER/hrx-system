// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/scf/combine_patterns.h"

#include "loom/ops/scf/canonicalize.h"
#include "loom/ops/scf/ops.h"

static iree_status_t loom_scf_select_combine_pattern(
    const loom_rewrite_pattern_t* pattern, void* context, loom_op_t* op,
    loom_rewriter_t* rewriter, bool* out_changed) {
  (void)pattern;
  (void)context;
  return loom_scf_select_combine_integer_extremum(op, rewriter, out_changed);
}

static const loom_rewrite_pattern_t kScfSourceCombinePatterns[] = {
    {
        .root_kind = LOOM_OP_SCF_SELECT,
        .match_and_rewrite = loom_scf_select_combine_pattern,
    },
};

const loom_rewrite_pattern_provider_t loom_scf_source_combine_pattern_provider =
    {
        .name = IREE_SVL("scf"),
        .patterns = kScfSourceCombinePatterns,
        .pattern_count = IREE_ARRAYSIZE(kScfSourceCombinePatterns),
};
