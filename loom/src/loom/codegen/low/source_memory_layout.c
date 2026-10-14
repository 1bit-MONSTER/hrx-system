// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/source_memory_layout.h"

#include "iree/base/internal/math.h"

bool loom_low_source_memory_query_byte_shift(int64_t byte_stride,
                                             uint32_t* out_shift) {
  *out_shift = LOOM_LOW_SOURCE_MEMORY_ACCESS_BYTE_SHIFT_NONE;
  if (byte_stride <= 0 || !iree_math_is_power_of_two_i64(byte_stride)) {
    return false;
  }
  *out_shift =
      (uint32_t)iree_math_count_trailing_zeros_u64((uint64_t)byte_stride);
  return true;
}

void loom_low_source_memory_query_axis_byte_stride(
    const loom_value_fact_table_t* fact_table,
    const loom_vector_memory_access_t* vector_access, uint8_t view_axis,
    loom_low_source_memory_axis_byte_stride_t* out_stride) {
  *out_stride = (loom_low_source_memory_axis_byte_stride_t){
      .kind = LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_UNAVAILABLE,
      .byte_facts = loom_value_facts_unknown(),
      .static_byte_shift = LOOM_LOW_SOURCE_MEMORY_ACCESS_BYTE_SHIFT_NONE,
  };
  if (view_axis >= vector_access->view_rank ||
      vector_access->static_element_byte_count < 0) {
    return;
  }

  int64_t static_byte_coefficient = vector_access->static_element_byte_count;
  loom_value_facts_t byte_facts =
      loom_value_facts_exact_i64(static_byte_coefficient);
  if (vector_access->layout_kind == LOOM_VECTOR_MEMORY_LAYOUT_DENSE) {
    for (uint8_t axis = (uint8_t)(view_axis + 1);
         axis < vector_access->view_rank; ++axis) {
      loom_value_facts_t dimension_facts = loom_value_facts_unknown();
      if (loom_type_dim_is_dynamic_at(vector_access->view_type, axis)) {
        const loom_value_id_t dimension_value =
            loom_type_dim_value_id_at(vector_access->view_type, axis);
        dimension_facts = loom_value_facts_non_negative_extent(
            loom_value_fact_table_lookup(fact_table, dimension_value));
        int64_t exact_dimension = 0;
        if (loom_value_facts_as_exact_i64(dimension_facts, &exact_dimension)) {
          if (!iree_checked_mul_i64(static_byte_coefficient, exact_dimension,
                                    &static_byte_coefficient)) {
            out_stride->kind = LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_INVALID;
            return;
          }
        } else {
          out_stride->dynamic_factors[out_stride->dynamic_factor_count++] =
              dimension_value;
        }
      } else {
        const int64_t dimension_size =
            loom_type_dim_static_size_at(vector_access->view_type, axis);
        dimension_facts = loom_value_facts_exact_i64(dimension_size);
        if (dimension_size < 0 ||
            !iree_checked_mul_i64(static_byte_coefficient, dimension_size,
                                  &static_byte_coefficient)) {
          out_stride->kind = LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_INVALID;
          return;
        }
      }
      loom_value_facts_muli(&byte_facts, &dimension_facts, &byte_facts);
    }
  } else if (vector_access->layout_kind == LOOM_VECTOR_MEMORY_LAYOUT_STRIDED) {
    const loom_value_fact_address_layout_t layout =
        vector_access->layout_summary;
    if (layout.kind != LOOM_VALUE_FACT_ADDRESS_LAYOUT_STRIDED ||
        view_axis >= layout.rank || !layout.strides) {
      return;
    }
    const loom_value_facts_t element_stride_facts = layout.strides[view_axis];
    if (loom_value_facts_is_float(element_stride_facts)) {
      out_stride->kind = LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_INVALID;
      return;
    }
    loom_value_facts_muli(&byte_facts, &element_stride_facts, &byte_facts);
    int64_t exact_element_stride = 0;
    if (loom_value_facts_as_exact_i64(element_stride_facts,
                                      &exact_element_stride)) {
      if (!iree_checked_mul_i64(static_byte_coefficient, exact_element_stride,
                                &static_byte_coefficient)) {
        out_stride->kind = LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_INVALID;
        return;
      }
    } else {
      const loom_value_fact_layout_strides_t stride_values =
          vector_access->layout_stride_values;
      if (view_axis >= stride_values.count ||
          stride_values.values[view_axis] == LOOM_VALUE_ID_INVALID) {
        out_stride->kind =
            LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_UNMATERIALIZED;
        out_stride->byte_facts = byte_facts;
        return;
      }
      out_stride->dynamic_factors[out_stride->dynamic_factor_count++] =
          stride_values.values[view_axis];
    }
  } else {
    return;
  }

  // A zero static factor makes the complete product exact even when a suffix
  // dimension is dynamic.
  if (static_byte_coefficient == 0) {
    out_stride->dynamic_factor_count = 0;
    byte_facts = loom_value_facts_exact_i64(0);
  }
  out_stride->kind = out_stride->dynamic_factor_count == 0
                         ? LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_STATIC
                         : LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_DYNAMIC;
  out_stride->static_byte_coefficient = static_byte_coefficient;
  out_stride->byte_facts = byte_facts;
  (void)loom_low_source_memory_query_byte_shift(static_byte_coefficient,
                                                &out_stride->static_byte_shift);
}

loom_low_source_memory_address_layout_t
loom_low_source_memory_classify_address_layout(
    const loom_vector_memory_access_t* vector_access) {
  if (vector_access->layout_kind == LOOM_VECTOR_MEMORY_LAYOUT_DENSE) {
    return LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_COMPACT_ROW_MAJOR;
  }
  if (vector_access->layout_kind != LOOM_VECTOR_MEMORY_LAYOUT_STRIDED) {
    return LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_UNPROVEN;
  }

  int64_t expected_stride = 1;
  for (int16_t axis = (int16_t)vector_access->view_rank - 1; axis >= 0;
       --axis) {
    // A singleton axis contributes no address delta, regardless of its stride.
    if (!loom_type_dim_is_dynamic_at(vector_access->view_type, (uint8_t)axis) &&
        loom_type_dim_static_size_at(vector_access->view_type, (uint8_t)axis) ==
            1) {
      continue;
    }
    int64_t actual_stride = 0;
    if (!loom_vector_memory_access_static_axis_stride(
            vector_access, (uint8_t)axis, &actual_stride) ||
        actual_stride != expected_stride) {
      return LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_UNPROVEN;
    }
    if (axis != 0) {
      if (loom_type_dim_is_dynamic_at(vector_access->view_type,
                                      (uint8_t)axis) ||
          !iree_checked_mul_i64(expected_stride,
                                loom_type_dim_static_size_at(
                                    vector_access->view_type, (uint8_t)axis),
                                &expected_stride)) {
        return LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_UNPROVEN;
      }
    }
  }
  return LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_COMPACT_ROW_MAJOR;
}
