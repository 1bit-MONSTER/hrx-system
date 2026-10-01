// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdint.h>

#include "loom/ir/module.h"
#include "loom/ops/low/capture.h"
#include "loom/ops/low/ops.h"
#include "loom/rewrite/rewriter.h"
#include "loom/target/registers.h"

static loom_op_t* loom_low_defining_op(loom_rewriter_t* rewriter,
                                       loom_value_id_t value_id) {
  loom_value_t* value = loom_module_value(rewriter->module, value_id);
  if (loom_value_is_block_arg(value)) {
    return NULL;
  }
  return loom_value_def_op(value);
}

// A projection captures contents, not a required alias of the original source.
// Keep a changing source's capture at its original point when composing
// projections. Stable sources can retain the outer observation point without
// extending lifetimes or perturbing the schedule.
static iree_status_t loom_low_slice_replace_at(loom_op_t* op,
                                               loom_rewriter_t* rewriter,
                                               loom_op_t* capture_op,
                                               loom_value_id_t source,
                                               int64_t offset) {
  const bool source_is_stable =
      loom_low_capture_source_is_stable(rewriter->module, source);
  const loom_type_t result_type =
      loom_module_value_type(rewriter->module, loom_low_slice_result(op));
  // Read-only identity projections need no intermediate owner. Prove this
  // before creating a capture so its construction and removal do not restart
  // fact propagation or enqueue the same users twice.
  if (source_is_stable && offset == 0 &&
      loom_type_equal(loom_module_value_type(rewriter->module, source),
                      result_type) &&
      loom_value_ownership_use_count(loom_module_value(
          rewriter->module, loom_low_slice_result(op))) == 0) {
    return loom_rewriter_replace_all_uses_and_erase(rewriter, op, &source, 1);
  }
  loom_builder_set_before(&rewriter->builder,
                          source_is_stable ? op : capture_op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(rewriter);
  loom_op_t* replacement_op = NULL;
  IREE_RETURN_IF_ERROR(loom_low_slice_build(&rewriter->builder, source, offset,
                                            result_type, op->location,
                                            &replacement_op));
  const loom_value_id_t replacement = loom_low_slice_result(replacement_op);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement,
                                                  1);
}

static loom_value_slice_t loom_low_slice_concat_sources(
    loom_rewriter_t* rewriter, loom_op_t* slice_op, loom_op_t* concat_op) {
  const uint32_t slice_offset = (uint32_t)loom_low_slice_offset(slice_op);
  const loom_type_t slice_type =
      loom_module_value_type(rewriter->module, loom_low_slice_result(slice_op));
  const uint32_t slice_end =
      slice_offset + loom_low_register_type_unit_count(slice_type);

  uint32_t source_offset = 0;
  loom_value_slice_t selected = {0};
  loom_value_slice_t sources = loom_low_concat_sources(concat_op);
  for (uint16_t i = 0; i < sources.count; ++i) {
    if (source_offset == slice_offset) {
      selected.values = sources.values + i;
    }
    const loom_value_t* source =
        loom_module_value(rewriter->module, sources.values[i]);
    source_offset += loom_low_register_type_unit_count(source->type);
    if (!selected.values) {
      if (source_offset > slice_offset) {
        return (loom_value_slice_t){0};
      }
      continue;
    }
    ++selected.count;
    if (selected.count == 1) {
      if (source_offset == slice_end) {
        return selected;
      }
      // Materialize only at a sole result-less sink. Storage chains can
      // repeatedly project a new concat, and dead result-producing consumers
      // can release the inputs for another projection. Both can duplicate
      // operand lists quadratically as intermediate operations are erased.
      const loom_use_t* use = loom_value_single_use(
          loom_module_value(rewriter->module, loom_low_slice_result(slice_op)));
      if (!use || loom_use_user_op(*use)->result_count != 0 ||
          iree_any_bit_set(loom_use_user_op(*use)->traits,
                           LOOM_TRAIT_STORAGE_RELATION)) {
        return (loom_value_slice_t){0};
      }
    }
    // A new sub-concat gives each selected input a second use while the
    // original concat is live. Requiring exclusive inputs keeps overlapping
    // projections from duplicating the same operand lists.
    if (source_offset > slice_end || !loom_value_has_single_use(source)) {
      return (loom_value_slice_t){0};
    }
    if (source_offset == slice_end) {
      return selected;
    }
  }
  return (loom_value_slice_t){0};
}

static iree_status_t loom_low_slice_canonicalize_concat_slice(
    loom_op_t* op, loom_rewriter_t* rewriter, loom_op_t* concat_op,
    bool* out_changed) {
  *out_changed = false;
  const loom_value_slice_t sources =
      loom_low_slice_concat_sources(rewriter, op, concat_op);
  if (sources.count == 0) {
    return iree_ok_status();
  }
  loom_value_id_t replacement = sources.values[0];
  if (sources.count > 1) {
    loom_builder_set_before(&rewriter->builder, op);
    loom_value_id_t value_checkpoint = loom_rewriter_value_checkpoint(rewriter);
    loom_op_t* replacement_op = NULL;
    IREE_RETURN_IF_ERROR(loom_low_concat_build(
        &rewriter->builder, sources.values, sources.count,
        loom_module_value_type(rewriter->module, loom_low_slice_result(op)),
        op->location, &replacement_op));
    replacement = loom_low_concat_result(replacement_op);
    IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
        rewriter, op, &replacement, 1, value_checkpoint));
  }
  IREE_RETURN_IF_ERROR(
      loom_low_slice_replace_at(op, rewriter, concat_op, replacement, 0));
  *out_changed = true;
  return iree_ok_status();
}

static iree_status_t loom_low_slice_canonicalize_nested_slice(
    loom_op_t* op, loom_rewriter_t* rewriter, loom_op_t* inner_slice_op,
    bool* out_changed) {
  *out_changed = false;
  const int64_t outer_offset = loom_low_slice_offset(op);
  const int64_t inner_offset = loom_low_slice_offset(inner_slice_op);
  if (outer_offset < 0 || inner_offset < 0 ||
      outer_offset > INT64_MAX - inner_offset) {
    return iree_ok_status();
  }

  const loom_value_id_t inner_source = loom_low_slice_source(inner_slice_op);
  const int64_t combined_offset = inner_offset + outer_offset;
  IREE_RETURN_IF_ERROR(loom_low_slice_replace_at(
      op, rewriter, inner_slice_op, inner_source, combined_offset));
  *out_changed = true;
  return iree_ok_status();
}

iree_status_t loom_low_slice_canonicalize(loom_op_t* op,
                                          loom_rewriter_t* rewriter) {
  const loom_value_id_t source = loom_low_slice_source(op);
  const loom_type_t source_type =
      loom_module_value_type(rewriter->module, source);
  const loom_type_t result_type =
      loom_module_value_type(rewriter->module, loom_low_slice_result(op));
  if (loom_low_slice_offset(op) == 0 &&
      loom_type_equal(source_type, result_type)) {
    if (loom_low_capture_can_forward(rewriter->module, source,
                                     loom_low_slice_result(op), 1)) {
      return loom_rewriter_replace_all_uses_and_erase(rewriter, op, &source, 1);
    }
    // An identity-width projection still separates subsequent ownership and
    // content changes. Allocation may coalesce it when its observations allow.
    return iree_ok_status();
  }

  loom_op_t* source_op = loom_low_defining_op(rewriter, source);
  if (loom_low_concat_isa(source_op)) {
    bool changed = false;
    IREE_RETURN_IF_ERROR(loom_low_slice_canonicalize_concat_slice(
        op, rewriter, source_op, &changed));
    if (changed) {
      return iree_ok_status();
    }
  }
  if (loom_low_slice_isa(source_op)) {
    bool changed = false;
    IREE_RETURN_IF_ERROR(loom_low_slice_canonicalize_nested_slice(
        op, rewriter, source_op, &changed));
    if (changed) {
      return iree_ok_status();
    }
  }
  return iree_ok_status();
}
