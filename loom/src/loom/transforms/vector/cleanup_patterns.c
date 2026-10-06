// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/vector/cleanup_patterns.h"

#include "loom/ir/module.h"
#include "loom/ops/special_values.h"
#include "loom/ops/vector/ops.h"
#include "loom/rewrite/rewriter.h"

static bool loom_vector_cleanup_type_has_poison(loom_type_t type) {
  if (loom_type_is_scalar(type)) {
    return true;
  }
  if (loom_type_is_vector(type)) {
    return !loom_type_has_static_zero_extent(type);
  }
  return false;
}

static bool loom_vector_extract_consumes_static_empty_axis(
    const loom_module_t* module, const loom_op_t* op) {
  const loom_type_t source_type =
      loom_module_value_type(module, loom_vector_extract_source(op));
  if (!loom_type_is_vector(source_type)) {
    return false;
  }

  const loom_attribute_t static_indices =
      loom_vector_extract_static_indices(op);
  if (static_indices.kind != LOOM_ATTR_I64_ARRAY) {
    return false;
  }

  const uint8_t source_rank = loom_type_rank(source_type);
  uint16_t consumed_rank = static_indices.count;
  if (consumed_rank > source_rank) {
    consumed_rank = source_rank;
  }
  for (uint16_t axis = 0; axis < consumed_rank; ++axis) {
    if (!loom_type_dim_is_dynamic_at(source_type, axis) &&
        loom_type_dim_static_size_at(source_type, axis) == 0) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_vector_empty_extract_pattern(
    const loom_rewrite_pattern_t* pattern, void* context, loom_op_t* op,
    loom_rewriter_t* rewriter, bool* out_changed) {
  (void)pattern;
  (void)context;
  *out_changed = false;
  if (!loom_vector_extract_consumes_static_empty_axis(rewriter->module, op)) {
    return iree_ok_status();
  }
  const loom_type_t result_type =
      loom_module_value_type(rewriter->module, loom_vector_extract_result(op));
  if (!loom_vector_cleanup_type_has_poison(result_type)) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_results_with_materialized_values_and_erase(
          rewriter, op, loom_poison_build));
  *out_changed = true;
  return iree_ok_status();
}

static iree_status_t loom_vector_replace_empty_accumulator_op(
    loom_rewriter_t* rewriter, loom_op_t* op, loom_value_id_t input,
    loom_value_id_t init, bool* out_changed) {
  *out_changed = false;
  const loom_type_t input_type =
      loom_module_value_type(rewriter->module, input);
  if (!loom_type_is_vector(input_type) ||
      !loom_type_has_static_zero_extent(input_type)) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_all_uses_and_erase(rewriter, op, &init, 1));
  *out_changed = true;
  return iree_ok_status();
}

static iree_status_t loom_vector_empty_reduce_pattern(
    const loom_rewrite_pattern_t* pattern, void* context, loom_op_t* op,
    loom_rewriter_t* rewriter, bool* out_changed) {
  (void)pattern;
  (void)context;
  return loom_vector_replace_empty_accumulator_op(
      rewriter, op, loom_vector_reduce_input(op), loom_vector_reduce_init(op),
      out_changed);
}

static iree_status_t loom_vector_empty_dotf_pattern(
    const loom_rewrite_pattern_t* pattern, void* context, loom_op_t* op,
    loom_rewriter_t* rewriter, bool* out_changed) {
  (void)pattern;
  (void)context;
  return loom_vector_replace_empty_accumulator_op(
      rewriter, op, loom_vector_dotf_lhs(op), loom_vector_dotf_init(op),
      out_changed);
}

static const loom_rewrite_pattern_t kVectorUniversalPreFoldPatterns[] = {
    {
        .root_kind = LOOM_OP_VECTOR_EXTRACT,
        .match_and_rewrite = loom_vector_empty_extract_pattern,
    },
    {
        .root_kind = LOOM_OP_VECTOR_REDUCE,
        .match_and_rewrite = loom_vector_empty_reduce_pattern,
    },
    {
        .root_kind = LOOM_OP_VECTOR_DOTF,
        .match_and_rewrite = loom_vector_empty_dotf_pattern,
    },
};

const loom_rewrite_pattern_provider_t
    loom_vector_universal_pre_fold_pattern_provider = {
        .name = IREE_SVL("vector"),
        .patterns = kVectorUniversalPreFoldPatterns,
        .pattern_count = IREE_ARRAYSIZE(kVectorUniversalPreFoldPatterns),
};
