// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_OPS_VECTOR_TABLE_H_
#define LOOM_OPS_VECTOR_TABLE_H_

#include "loom/rewrite/rewriter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Combines vector.from_elements lanes extracted from one rank-one register
// table into vector.table.lookup. At least one selector must be dynamic;
// entirely static reconstruction belongs to the ordinary lane/shuffle folds.
// Nonmatching constructions are unchanged and allocate no scratch storage.
// Enable only before target legalization so cleanup cannot recreate a lookup
// that the target has deliberately decomposed into scalar extracts.
iree_status_t loom_vector_from_elements_to_table_lookup(
    loom_op_t* op, loom_rewriter_t* rewriter, bool* out_changed);

// Removes value-preserving promotions from a lookup's constructed index
// vector, consuming retained numeric facts. Run before target legalization:
// changing the index representation can change native lookup eligibility.
iree_status_t loom_vector_table_lookup_simplify_indices(
    loom_op_t* op, loom_rewriter_t* rewriter, bool* out_changed);

// Replaces an equal-shaped table lookup whose retained index facts identify
// every in-bounds lane with vector.shuffle. The static permutation then
// reaches target-native structural lowering without retaining an index value.
iree_status_t loom_vector_table_lookup_to_shuffle(loom_op_t* op,
                                                  loom_rewriter_t* rewriter,
                                                  bool* out_changed);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_OPS_VECTOR_TABLE_H_
