// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/gather.h"

#include <stdint.h>

#include "loom/ir/facts.h"
#include "loom/ir/module.h"
#include "loom/ir/scalar_type.h"
#include "loom/ops/global/ops.h"
#include "loom/ops/vector/ops.h"
#include "loom/util/fact_extensions.h"

// One derived payload must fit entirely within one AIE2P programmer bank.
#define LOOM_AIE2P_GATHER_BANK_CAPACITY 16384u

static bool loom_aie2p_gather_type_is_vector(loom_type_t type,
                                             loom_scalar_type_t element_type,
                                             int64_t lane_count) {
  return loom_type_is_vector(type) && loom_type_rank(type) == 1 &&
         loom_type_element_type(type) == element_type &&
         !loom_type_dim_is_dynamic_at(type, 0) &&
         loom_type_dim_static_size_at(type, 0) == lane_count;
}

static uint8_t loom_aie2p_gather_element_bit_count(
    loom_scalar_type_t element_type) {
  switch (element_type) {
    case LOOM_SCALAR_TYPE_I16:
    case LOOM_SCALAR_TYPE_F16:
    case LOOM_SCALAR_TYPE_BF16:
      return 16;
    case LOOM_SCALAR_TYPE_I32:
    case LOOM_SCALAR_TYPE_F32:
      return 32;
    case LOOM_SCALAR_TYPE_I64:
    case LOOM_SCALAR_TYPE_F64:
      return 64;
    default:
      return 0;
  }
}

bool loom_aie2p_match_immutable_gather(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    const loom_op_t* source_op,
    const loom_low_source_memory_access_plan_t* access,
    loom_aie2p_immutable_gather_match_t* out_match) {
  *out_match = (loom_aie2p_immutable_gather_match_t){0};
  if (!loom_vector_gather_isa(source_op) || access == NULL ||
      access->operation_kind != LOOM_LOW_SOURCE_MEMORY_OPERATION_LOAD ||
      access->memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_CONSTANT ||
      access->address_layout !=
          LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_COMPACT_ROW_MAJOR ||
      access->access_flags != 0 || access->static_byte_offset != 0 ||
      access->physical_root_byte_offset != 0 ||
      access->static_view_base_byte_offset != 0 ||
      access->base_view_value_id != access->view_value_id ||
      access->dynamic_term_count != 0 ||
      access->dynamic_view_base_value_id != LOOM_VALUE_ID_INVALID ||
      access->source_index_static_offset_extracted) {
    return false;
  }

  const loom_value_id_t view_value_id = loom_vector_gather_view(source_op);
  const loom_type_t view_type = loom_module_value_type(module, view_value_id);
  const loom_type_t offsets_type =
      loom_module_value_type(module, loom_vector_gather_offsets(source_op));
  const loom_type_t result_type =
      loom_module_value_type(module, loom_vector_gather_result(source_op));
  if (!loom_type_is_view(view_type) || loom_type_rank(view_type) != 1 ||
      loom_type_dim_is_dynamic_at(view_type, 0)) {
    return false;
  }
  const loom_scalar_type_t element_type = loom_type_element_type(view_type);
  const uint8_t element_bit_count =
      loom_aie2p_gather_element_bit_count(element_type);
  if (element_bit_count == 0) {
    return false;
  }
  const uint32_t element_byte_count = element_bit_count / 8;
  const uint32_t result_lane_count = 64 / element_byte_count;
  if (access->element_byte_count != element_byte_count ||
      access->vector_lane_count != result_lane_count ||
      access->vector_lane_byte_stride != element_byte_count ||
      !loom_aie2p_gather_type_is_vector(offsets_type, LOOM_SCALAR_TYPE_I32,
                                        result_lane_count) ||
      !loom_aie2p_gather_type_is_vector(result_type, element_type,
                                        result_lane_count)) {
    return false;
  }
  const int64_t element_count = loom_type_dim_static_size_at(view_type, 0);
  if (element_count <= 0 || element_count > UINT32_MAX ||
      (element_count % (32 / element_byte_count)) != 0) {
    return false;
  }

  const loom_attribute_t static_indices =
      loom_vector_gather_static_indices(source_op);
  if (loom_vector_gather_indices(source_op).count != 0 ||
      static_indices.kind != LOOM_ATTR_I64_ARRAY || static_indices.count != 1 ||
      static_indices.i64_array[0] != 0) {
    return false;
  }

  const loom_symbol_ref_t source_symbol = access->root_symbol;
  if (!loom_symbol_ref_is_valid(source_symbol) ||
      source_symbol.module_id != 0 ||
      source_symbol.symbol_id >= module->symbols.count) {
    return false;
  }
  const loom_op_t* source_definition =
      module->symbols.entries[source_symbol.symbol_id].defining_op;
  if (!loom_global_rodata_def_isa(source_definition)) {
    return false;
  }

  const iree_const_byte_span_t source_contents =
      loom_global_rodata_def_contents(source_definition);
  const uint64_t logical_byte_length =
      (uint64_t)element_count * element_byte_count;
  if (source_contents.data_length != logical_byte_length ||
      logical_byte_length * 2 > LOOM_AIE2P_GATHER_BANK_CAPACITY) {
    return false;
  }

  int64_t lower_bound = 0;
  int64_t upper_bound = 0;
  const loom_value_facts_t offset_facts = loom_value_fact_table_lookup(
      fact_table, loom_vector_gather_offsets(source_op));
  if (!loom_value_facts_query_vector_integer_bounds(
          &fact_table->context, offset_facts,
          /*maximum_lane_count=*/result_lane_count, &lower_bound,
          &upper_bound) ||
      lower_bound < 0 || upper_bound >= element_count) {
    return false;
  }

  *out_match = (loom_aie2p_immutable_gather_match_t){
      .source_symbol = source_symbol,
      .source_contents = source_contents,
      .element_bit_count = element_bit_count,
  };
  return true;
}

iree_status_t loom_aie2p_query_gather_contract(
    void* user_data,
    const loom_target_contract_query_environment_t* environment,
    const loom_op_t* source_op,
    loom_target_contract_query_result_t* out_result) {
  (void)user_data;
  *out_result = loom_target_contract_query_result_empty();
  if (!loom_vector_gather_isa(source_op)) {
    return iree_ok_status();
  }

  loom_low_source_memory_access_plan_t access = {0};
  loom_low_source_memory_access_diagnostic_t diagnostic = {0};
  loom_aie2p_immutable_gather_match_t match = {0};
  if (loom_low_source_memory_access_plan_build(
          environment->view_regions, source_op, &access, &diagnostic) &&
      loom_aie2p_match_immutable_gather(environment->module,
                                        environment->fact_table, source_op,
                                        &access, &match)) {
    out_result->outcome = LOOM_TARGET_CONTRACT_QUERY_LEGAL;
  }
  return iree_ok_status();
}
