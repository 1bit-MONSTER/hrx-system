// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/wasm/lower/vector_dynamic_slice.h"

#include "loom/ir/module.h"
#include "loom/ops/vector/ops.h"
#include "loom/util/fact_vector_partition.h"

bool loom_wasm_dynamic_slice_values(const loom_module_t* module,
                                    const loom_value_fact_table_t* fact_table,
                                    const loom_op_t* source_op,
                                    loom_value_id_t* out_source,
                                    loom_value_id_t* out_result) {
  *out_source = LOOM_VALUE_ID_INVALID;
  *out_result = LOOM_VALUE_ID_INVALID;
  if (!loom_vector_slice_isa(source_op)) {
    return false;
  }
  const loom_value_slice_t dynamic_offsets =
      loom_vector_slice_offsets(source_op);
  const loom_attribute_t static_offsets =
      loom_vector_slice_static_offsets(source_op);
  if (dynamic_offsets.count != 1 ||
      static_offsets.kind != LOOM_ATTR_I64_ARRAY || static_offsets.count != 1 ||
      static_offsets.i64_array[0] != INT64_MIN) {
    return false;
  }
  const loom_value_id_t source = loom_vector_slice_source(source_op);
  if (loom_value_fact_table_query_vector_partition(fact_table, source).count ==
      0) {
    return false;
  }
  const loom_value_id_t result = loom_vector_slice_result(source_op);
  const loom_type_t source_type = loom_module_value_type(module, source);
  const loom_type_t result_type = loom_module_value_type(module, result);
  if (loom_type_rank(source_type) != 1 || loom_type_rank(result_type) != 1 ||
      !loom_type_element_type_equals(source_type, result_type)) {
    return false;
  }
  *out_source = source;
  *out_result = result;
  return true;
}

uint32_t loom_wasm_dynamic_slice_comparison_count(
    loom_wasm_vector_carrier_t source_carrier,
    loom_wasm_vector_carrier_t result_carrier) {
  const uint32_t maximum_byte_offset =
      source_carrier.payload_byte_count - result_carrier.payload_byte_count;
  return maximum_byte_offset / LOOM_WASM_VECTOR_CARRIER_PACKET_BYTE_COUNT;
}

uint32_t loom_wasm_dynamic_slice_operation_count(
    loom_wasm_vector_carrier_t source_carrier,
    loom_wasm_vector_carrier_t result_carrier) {
  const uint32_t comparison_count =
      loom_wasm_dynamic_slice_comparison_count(source_carrier, result_carrier);
  const uint32_t descriptor_operation_count =
      8u + (source_carrier.element_bit_count == 8 ? 0u : 2u) +
      (comparison_count == 0 ? 0u : 1u) + 4u * comparison_count +
      (3u + 2u * comparison_count) * result_carrier.packet_count;
  const uint32_t structural_operation_count =
      (source_carrier.packet_count == 1 ? 0u : source_carrier.packet_count) +
      (result_carrier.packet_count == 1 ? 0u : 1u);
  return descriptor_operation_count + structural_operation_count;
}

bool loom_wasm_dynamic_slice_operation_count_is_bounded(
    loom_wasm_vector_carrier_t source_carrier,
    loom_wasm_vector_carrier_t result_carrier) {
  return loom_wasm_dynamic_slice_operation_count(source_carrier,
                                                 result_carrier) <=
         LOOM_WASM_DYNAMIC_SLICE_MAX_OPERATION_COUNT;
}
