// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/sanitizer/ops.h"
#include "loom/rewrite/rewriter.h"
#include "loom/util/predicate_facts.h"

iree_status_t loom_sanitizer_assert_value_canonicalize(
    loom_op_t* op, loom_rewriter_t* rewriter) {
  loom_value_slice_t values = loom_sanitizer_assert_value_values(op);
  const loom_attribute_t predicates =
      loom_sanitizer_assert_value_predicates(op);
  if (!loom_value_fact_table_proves_predicate_list(
          rewriter->fact_table, predicates.predicate_list, predicates.count)) {
    return iree_ok_status();
  }
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, values.values,
                                                  values.count);
}

iree_status_t loom_sanitizer_assert_op_canonicalize(loom_op_t* op,
                                                    loom_rewriter_t* rewriter) {
  const loom_attribute_t predicates = loom_sanitizer_assert_op_predicates(op);
  if (!loom_value_fact_table_proves_predicate_list(
          rewriter->fact_table, predicates.predicate_list, predicates.count)) {
    return iree_ok_status();
  }
  return loom_rewriter_erase(rewriter, op);
}
