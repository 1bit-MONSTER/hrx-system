// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/vector/table.h"

#include "loom/ir/facts.h"
#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/vector/ops.h"

static loom_op_t* loom_vector_table_defining_op(const loom_rewriter_t* rewriter,
                                                loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(rewriter->module, value_id);
  return loom_value_is_block_arg(value) ? NULL : loom_value_def_op(value);
}

static iree_status_t loom_vector_table_replace(
    loom_op_t* op, loom_rewriter_t* rewriter, loom_value_id_t indices,
    loom_value_id_t table, loom_value_id_t value_checkpoint) {
  const loom_type_t result_type =
      loom_module_value_type(rewriter->module, loom_op_const_results(op)[0]);
  loom_op_t* lookup = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_table_lookup_build(
      &rewriter->builder, table, indices, result_type, op->location, &lookup));
  const loom_value_id_t replacement = loom_vector_table_lookup_result(lookup);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement,
                                                  1);
}

iree_status_t loom_vector_from_elements_to_table_lookup(
    loom_op_t* op, loom_rewriter_t* rewriter, bool* out_changed) {
  *out_changed = false;
  const loom_value_slice_t elements = loom_vector_from_elements_elements(op);
  loom_value_id_t table = LOOM_VALUE_ID_INVALID;
  bool has_dynamic_index = false;
  for (uint16_t i = 0; i < elements.count; ++i) {
    const loom_op_t* extract =
        loom_vector_table_defining_op(rewriter, elements.values[i]);
    if (!extract || !loom_vector_extract_isa(extract)) {
      return iree_ok_status();
    }
    const loom_value_id_t source = loom_vector_extract_source(extract);
    if (i == 0) {
      table = source;
      if (loom_type_rank(loom_module_value_type(rewriter->module, table)) !=
          1) {
        return iree_ok_status();
      }
    } else if (source != table) {
      return iree_ok_status();
    }
    has_dynamic_index |= loom_vector_extract_indices(extract).count != 0;
  }
  if (!has_dynamic_index) {
    return iree_ok_status();
  }

  loom_value_id_t* indices = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      rewriter->arena, elements.count, sizeof(*indices), (void**)&indices));
  loom_builder_set_before(&rewriter->builder, op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(rewriter);
  const loom_type_t index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  for (uint16_t i = 0; i < elements.count; ++i) {
    const loom_op_t* extract =
        loom_vector_table_defining_op(rewriter, elements.values[i]);
    const loom_value_slice_t dynamic_indices =
        loom_vector_extract_indices(extract);
    if (dynamic_indices.count) {
      indices[i] = dynamic_indices.values[0];
    } else {
      loom_op_t* constant = NULL;
      IREE_RETURN_IF_ERROR(loom_index_constant_build(
          &rewriter->builder,
          loom_attr_i64(
              loom_vector_extract_static_indices(extract).i64_array[0]),
          index_type, op->location, &constant));
      indices[i] = loom_index_constant_result(constant);
    }
  }
  loom_type_t indices_type = loom_module_value_type(
      rewriter->module, loom_vector_from_elements_result(op));
  indices_type.header = loom_type_make_header(
      LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_INDEX, loom_type_rank(indices_type),
      loom_type_flags(indices_type));
  indices_type.encoding_id = 0;
  indices_type.encoding_flags = 0;
  loom_op_t* construction = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_from_elements_build(
      &rewriter->builder, indices, elements.count, indices_type, op->location,
      &construction));
  IREE_RETURN_IF_ERROR(loom_vector_table_replace(
      op, rewriter, loom_vector_from_elements_result(construction), table,
      value_checkpoint));
  *out_changed = true;
  return iree_ok_status();
}

// Table indices use the same signed numeric interpretation as index.cast.
// Remove only value-preserving promotions. In particular, zero-extending an
// arbitrary byte is different from interpreting that byte as a signed index.
// Removing an address conversion also requires its input to fit the smallest
// supported index carrier, so target representability checks are preserved.
static loom_value_id_t loom_vector_table_unpromoted_index(
    const loom_rewriter_t* rewriter, loom_value_id_t value) {
  const loom_op_t* definition = loom_vector_table_defining_op(rewriter, value);
  if (definition && loom_index_cast_isa(definition)) {
    const loom_value_id_t input = loom_index_cast_input(definition);
    const loom_scalar_type_t input_type =
        loom_type_element_type(loom_module_value_type(rewriter->module, input));
    if (loom_type_element_type(loom_module_value_type(
            rewriter->module, value)) != LOOM_SCALAR_TYPE_INDEX ||
        !loom_scalar_type_is_integer(input_type) ||
        input_type == LOOM_SCALAR_TYPE_I1 ||
        !loom_value_facts_fit_signed_bit_count(
            loom_rewriter_value_facts(rewriter, input), 32)) {
      return value;
    }
    value = input;
    definition = loom_vector_table_defining_op(rewriter, value);
  }
  if (definition && (loom_scalar_extsi_isa(definition) ||
                     loom_scalar_extui_isa(definition))) {
    const loom_value_id_t input = loom_op_const_operands(definition)[0];
    if (loom_type_element_type(loom_module_value_type(
            rewriter->module, input)) != LOOM_SCALAR_TYPE_I1 &&
        (loom_scalar_extsi_isa(definition) ||
         loom_value_facts_is_non_negative(
             loom_rewriter_value_facts(rewriter, input)))) {
      value = input;
    }
  }
  return value;
}

iree_status_t loom_vector_table_lookup_simplify_indices(
    loom_op_t* op, loom_rewriter_t* rewriter, bool* out_changed) {
  *out_changed = false;
  const loom_value_id_t indices = loom_vector_table_lookup_indices(op);
  const loom_op_t* construction =
      loom_vector_table_defining_op(rewriter, indices);
  if (!construction || !loom_vector_from_elements_isa(construction)) {
    return iree_ok_status();
  }
  const loom_value_slice_t elements =
      loom_vector_from_elements_elements(construction);
  loom_scalar_type_t element_type = LOOM_SCALAR_TYPE_NONE;
  int64_t constant_minimum = 0;
  int64_t constant_maximum = 0;
  for (uint16_t i = 0; i < elements.count; ++i) {
    int64_t constant = 0;
    if (loom_value_facts_as_exact_i64(
            loom_rewriter_value_facts(rewriter, elements.values[i]),
            &constant)) {
      constant_minimum = iree_min(constant_minimum, constant);
      constant_maximum = iree_max(constant_maximum, constant);
      continue;
    }
    const loom_value_id_t value =
        loom_vector_table_unpromoted_index(rewriter, elements.values[i]);
    const loom_scalar_type_t candidate_type =
        loom_type_element_type(loom_module_value_type(rewriter->module, value));
    if (element_type == LOOM_SCALAR_TYPE_NONE) {
      element_type = candidate_type;
    } else if (candidate_type != element_type) {
      return iree_ok_status();
    }
  }
  const loom_type_t old_type =
      loom_module_value_type(rewriter->module, indices);
  if (element_type == LOOM_SCALAR_TYPE_NONE ||
      element_type == loom_type_element_type(old_type)) {
    return iree_ok_status();
  }
  int64_t domain_minimum = 0;
  int64_t domain_maximum = 0;
  loom_value_facts_scalar_type_domain(element_type, &domain_minimum,
                                      &domain_maximum);
  if (constant_minimum < domain_minimum || constant_maximum > domain_maximum) {
    return iree_ok_status();
  }

  loom_value_id_t* narrowed = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      rewriter->arena, elements.count, sizeof(*narrowed), (void**)&narrowed));
  loom_builder_set_before(&rewriter->builder, op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(rewriter);
  for (uint16_t i = 0; i < elements.count; ++i) {
    int64_t constant = 0;
    if (loom_value_facts_as_exact_i64(
            loom_rewriter_value_facts(rewriter, elements.values[i]),
            &constant)) {
      loom_op_t* constant_op = NULL;
      IREE_RETURN_IF_ERROR(loom_scalar_constant_build(
          &rewriter->builder, loom_attr_i64(constant),
          loom_type_scalar(element_type), op->location, &constant_op));
      narrowed[i] = loom_scalar_constant_result(constant_op);
    } else {
      narrowed[i] =
          loom_vector_table_unpromoted_index(rewriter, elements.values[i]);
    }
  }
  loom_type_t narrowed_type = old_type;
  narrowed_type.header = loom_type_make_header(LOOM_TYPE_VECTOR, element_type,
                                               loom_type_rank(old_type),
                                               loom_type_flags(old_type));
  narrowed_type.encoding_id = 0;
  narrowed_type.encoding_flags = 0;
  loom_op_t* narrowed_construction = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_from_elements_build(
      &rewriter->builder, narrowed, elements.count, narrowed_type, op->location,
      &narrowed_construction));
  IREE_RETURN_IF_ERROR(loom_vector_table_replace(
      op, rewriter, loom_vector_from_elements_result(narrowed_construction),
      loom_vector_table_lookup_table(op), value_checkpoint));
  *out_changed = true;
  return iree_ok_status();
}

iree_status_t loom_vector_table_lookup_to_shuffle(loom_op_t* op,
                                                  loom_rewriter_t* rewriter,
                                                  bool* out_changed) {
  *out_changed = false;
  if (rewriter->fact_table == NULL) {
    return iree_ok_status();
  }
  const loom_value_id_t table = loom_vector_table_lookup_table(op);
  const loom_value_id_t indices = loom_vector_table_lookup_indices(op);
  const loom_value_id_t result = loom_vector_table_lookup_result(op);
  const loom_type_t table_type =
      loom_module_value_type(rewriter->module, table);
  const loom_type_t result_type =
      loom_module_value_type(rewriter->module, result);
  if (!loom_type_equal(table_type, result_type)) {
    return iree_ok_status();
  }

  uint64_t table_count = 0;
  if (!loom_type_static_element_count(table_type, &table_count)) {
    return iree_ok_status();
  }
  const loom_value_facts_t index_facts =
      loom_rewriter_value_facts(rewriter, indices);
  loom_value_fact_small_static_lanes_t static_indices = {0};
  if (!loom_value_facts_query_small_static_lanes(
          &rewriter->fact_table->context, index_facts, &static_indices) ||
      static_indices.count != table_count) {
    return iree_ok_status();
  }

  int64_t source_lanes[LOOM_VALUE_FACT_SMALL_STATIC_LANE_LIMIT];
  for (iree_host_size_t lane = 0; lane < static_indices.count; ++lane) {
    if (!loom_value_facts_as_exact_i64(static_indices.lanes[lane],
                                       &source_lanes[lane]) ||
        source_lanes[lane] < 0 || (uint64_t)source_lanes[lane] >= table_count) {
      return iree_ok_status();
    }
  }

  loom_builder_set_before(&rewriter->builder, op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(rewriter);
  loom_op_t* shuffle = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_shuffle_build(
      &rewriter->builder, source_lanes, static_indices.count, table,
      result_type, op->location, &shuffle));
  const loom_value_id_t replacement = loom_vector_shuffle_result(shuffle);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement, 1));
  *out_changed = true;
  return iree_ok_status();
}
