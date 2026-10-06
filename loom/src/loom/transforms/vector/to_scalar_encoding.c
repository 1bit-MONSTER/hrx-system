// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/vector/to_scalar_encoding.h"

#include "loom/analysis/contract.h"
#include "loom/ir/module.h"
#include "loom/ir/scalar_type.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/scf/ops.h"
#include "loom/ops/vector/ops.h"
#include "loom/util/numeric_format.h"

//===----------------------------------------------------------------------===//
// Schema support
//===----------------------------------------------------------------------===//

static bool loom_vector_to_scalar_flags_are_single_or_none(uint64_t flags) {
  return flags == 0 || (flags & (flags - 1)) == 0;
}

static bool loom_vector_to_scalar_one_of_flags_are_supported(
    uint64_t flags, uint64_t supported) {
  return (flags & ~supported) == 0 &&
         loom_vector_to_scalar_flags_are_single_or_none(flags);
}

static bool loom_vector_to_scalar_bitset_flags_are_supported(
    uint64_t flags, uint64_t supported) {
  return (flags & ~supported) == 0;
}

static bool loom_vector_to_scalar_encoded_schema_uses_codebook(
    loom_value_fact_encoded_operand_schema_t schema) {
  return schema.codebook_policy ==
         LOOM_VALUE_FACT_CODEBOOK_POLICY_DYNAMIC_TABLE_OPERAND;
}

static bool loom_vector_to_scalar_encoded_schema_has_scale(
    loom_value_fact_encoded_operand_schema_t schema) {
  return schema.scale_operand_count != 0;
}

static bool loom_vector_to_scalar_encoded_schema_has_scale_affine(
    loom_value_fact_encoded_operand_schema_t schema) {
  return iree_any_bit_set(
      schema.affine_policy,
      LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_ONLY |
          LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_MIN |
          LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_BIAS |
          LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_ZERO_POINT);
}

static bool loom_vector_to_scalar_encoded_schema_uses_e8m0_scale(
    loom_value_fact_encoded_operand_schema_t schema) {
  return schema.scale_format == LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E8M0;
}

static loom_type_t loom_vector_to_scalar_encoded_arithmetic_lane_type(
    const loom_vector_to_scalar_encoded_operand_t* operand) {
  // E8M0 spans the full F32 exponent range. Applying it in a narrower logical
  // type can first overflow the finite scale to infinity, changing cases such
  // as a zero payload into NaN before the decoded value is rounded. Preserve
  // the scale's numeric contract in F32 and round the final affine result once.
  return loom_vector_to_scalar_encoded_schema_uses_e8m0_scale(operand->schema)
             ? loom_type_scalar(LOOM_SCALAR_TYPE_F32)
             : operand->logical_lane_type;
}

enum {
  LOOM_VECTOR_TO_SCALAR_E8M0_BYTES_PER_WORD = 4,
  LOOM_VECTOR_TO_SCALAR_E8M0_BYTE_BITS = 8,
  LOOM_VECTOR_TO_SCALAR_E8M0_BYTE_MASK = 0xFF,
  LOOM_VECTOR_TO_SCALAR_E8M0_F32_EXPONENT_SHIFT = 23,
  LOOM_VECTOR_TO_SCALAR_E8M0_MINIMUM_F32_BITS = 0x00400000,
  LOOM_VECTOR_TO_SCALAR_E8M0_QUIET_NAN_F32_BITS = 0x7FC00000,
};

static bool loom_vector_to_scalar_encoded_schema_auxiliary_is_supported(
    loom_value_fact_encoded_operand_schema_t schema,
    loom_encoding_auxiliary_view_t auxiliary) {
  loom_encoding_auxiliary_key_flags_t required_keys = 0;
  if (!loom_encoding_auxiliary_required_keys_from_schema(schema, &required_keys,
                                                         NULL)) {
    return false;
  }
  if ((auxiliary.present_keys & required_keys) != required_keys) {
    return false;
  }
  return (auxiliary.present_keys & ~required_keys) == 0;
}

static bool loom_vector_to_scalar_encoded_schema_is_supported(
    loom_value_fact_encoded_operand_schema_t schema) {
  if (loom_value_fact_encoded_operand_schema_is_unknown(schema)) {
    return false;
  }
  if (schema.payload_packing != LOOM_VALUE_FACT_PAYLOAD_PACKING_DENSE_LANES) {
    return false;
  }
  if (!loom_vector_to_scalar_one_of_flags_are_supported(
          schema.element_format,
          LOOM_VALUE_FACT_NUMERIC_FORMAT_F64 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_F32 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_F16 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_BF16 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E4M3 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E4M3FN |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E4M3FNUZ |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E5M2 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E5M2FNUZ |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_I32 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_U32 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_I16 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_U16 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_I8 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_U8 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_I1 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_U1 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_QUANT_I8 |
              LOOM_VALUE_FACT_NUMERIC_FORMAT_CODEBOOK_INDEX)) {
    return false;
  }
  if (!loom_vector_to_scalar_one_of_flags_are_supported(
          schema.scale_format, LOOM_VALUE_FACT_NUMERIC_FORMAT_NONE |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_F32 |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_F16 |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_BF16 |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E4M3 |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E4M3FN |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E4M3FNUZ |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E5M2 |
                                   LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E8M0)) {
    return false;
  }
  if (schema.secondary_scale_format != LOOM_VALUE_FACT_NUMERIC_FORMAT_NONE) {
    return false;
  }
  if (!loom_vector_to_scalar_one_of_flags_are_supported(
          schema.scale_topology,
          LOOM_VALUE_FACT_SCALE_TOPOLOGY_NONE |
              LOOM_VALUE_FACT_SCALE_TOPOLOGY_TENSOR_GLOBAL |
              LOOM_VALUE_FACT_SCALE_TOPOLOGY_ROW |
              LOOM_VALUE_FACT_SCALE_TOPOLOGY_COLUMN |
              LOOM_VALUE_FACT_SCALE_TOPOLOGY_GROUP_1D |
              LOOM_VALUE_FACT_SCALE_TOPOLOGY_BLOCK_1D |
              LOOM_VALUE_FACT_SCALE_TOPOLOGY_BLOCK_2D)) {
    return false;
  }
  if (!loom_vector_to_scalar_one_of_flags_are_supported(
          schema.affine_policy,
          LOOM_VALUE_FACT_AFFINE_POLICY_NONE |
              LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_ONLY |
              LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_MIN |
              LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_BIAS |
              LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_ZERO_POINT)) {
    return false;
  }
  if (!loom_vector_to_scalar_bitset_flags_are_supported(
          schema.rounding_policy, LOOM_VALUE_FACT_ROUNDING_POLICY_ALL) ||
      schema.sparsity_policy != LOOM_VALUE_FACT_SPARSITY_POLICY_NONE ||
      schema.scale_operand_count > 1) {
    return false;
  }
  if (!loom_vector_to_scalar_one_of_flags_are_supported(
          schema.codebook_policy,
          LOOM_VALUE_FACT_CODEBOOK_POLICY_NONE |
              LOOM_VALUE_FACT_CODEBOOK_POLICY_DYNAMIC_TABLE_OPERAND)) {
    return false;
  }
  if (loom_vector_to_scalar_encoded_schema_has_scale(schema)) {
    if (!loom_vector_to_scalar_encoded_schema_has_scale_affine(schema) ||
        schema.scale_topology == LOOM_VALUE_FACT_SCALE_TOPOLOGY_NONE ||
        schema.scale_format == LOOM_VALUE_FACT_NUMERIC_FORMAT_NONE ||
        !loom_value_fact_encoded_operand_schema_scale_is_complete(schema)) {
      return false;
    }
  } else if (schema.scale_topology != LOOM_VALUE_FACT_SCALE_TOPOLOGY_NONE ||
             loom_vector_to_scalar_encoded_schema_has_scale_affine(schema)) {
    return false;
  }
  if (loom_vector_to_scalar_encoded_schema_uses_codebook(schema) &&
      schema.element_format != LOOM_VALUE_FACT_NUMERIC_FORMAT_CODEBOOK_INDEX) {
    return false;
  }
  return true;
}

static bool loom_vector_to_scalar_numeric_lane_cast_is_supported(
    loom_type_t input_type, loom_type_t result_type) {
  if (!loom_type_is_scalar(input_type) || !loom_type_is_scalar(result_type)) {
    return false;
  }
  if (loom_type_equal(input_type, result_type)) {
    return true;
  }

  loom_scalar_type_t input_scalar_type = loom_type_element_type(input_type);
  loom_scalar_type_t result_scalar_type = loom_type_element_type(result_type);
  int32_t input_width = loom_scalar_type_bitwidth(input_scalar_type);
  int32_t result_width = loom_scalar_type_bitwidth(result_scalar_type);
  if (input_width <= 0 || result_width <= 0) {
    return false;
  }

  if (loom_scalar_type_is_float(input_scalar_type) &&
      loom_scalar_type_is_float(result_scalar_type)) {
    return input_width != result_width;
  }
  if (loom_scalar_type_is_integer(input_scalar_type) &&
      loom_scalar_type_is_integer(result_scalar_type)) {
    return true;
  }
  return loom_scalar_type_is_integer(input_scalar_type) &&
         loom_scalar_type_is_float(result_scalar_type);
}

static bool loom_vector_to_scalar_encoded_auxiliary_lane_type(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_encoding_auxiliary_key_t key, loom_type_t* out_lane_type) {
  loom_value_id_t value = operand->auxiliary.values[key];
  if (value == LOOM_VALUE_ID_INVALID ||
      value >= state->rewriter->module->values.count) {
    return false;
  }
  loom_type_t value_type =
      loom_module_value_type(state->rewriter->module, value);
  if (!loom_type_is_vector(value_type) || loom_type_rank(value_type) != 1) {
    return false;
  }
  *out_lane_type = loom_vector_to_scalar_lane_type(value_type);
  return true;
}

static bool loom_vector_to_scalar_encoded_auxiliary_lane_cast_is_supported(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_encoding_auxiliary_key_t key, loom_type_t result_type) {
  loom_type_t lane_type = {0};
  return loom_vector_to_scalar_encoded_auxiliary_lane_type(state, operand, key,
                                                           &lane_type) &&
         loom_vector_to_scalar_numeric_lane_cast_is_supported(lane_type,
                                                              result_type);
}

static bool loom_vector_to_scalar_encoded_auxiliary_matches_format(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_encoding_auxiliary_key_t key,
    loom_value_fact_numeric_format_flags_t format, loom_type_t result_type) {
  loom_scalar_type_t expected_scalar_type = LOOM_SCALAR_TYPE_NONE;
  if (!loom_numeric_format_direct_scalar_type(format, &expected_scalar_type)) {
    return false;
  }
  loom_type_t lane_type = {0};
  return loom_vector_to_scalar_encoded_auxiliary_lane_type(state, operand, key,
                                                           &lane_type) &&
         loom_type_element_type(lane_type) == expected_scalar_type &&
         loom_vector_to_scalar_numeric_lane_cast_is_supported(lane_type,
                                                              result_type);
}

static bool loom_vector_to_scalar_encoded_e8m0_auxiliary_is_supported(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_encoding_auxiliary_key_t key) {
  loom_type_t lane_type = {0};
  if (!loom_vector_to_scalar_encoded_auxiliary_lane_type(state, operand, key,
                                                         &lane_type) ||
      loom_type_element_type(lane_type) != LOOM_SCALAR_TYPE_I32) {
    return false;
  }

  const loom_value_id_t value = operand->auxiliary.values[key];
  const loom_type_t value_type =
      loom_module_value_type(state->rewriter->module, value);
  if (loom_type_dim_is_dynamic_at(value_type, 0) ||
      operand->blocks.is_dynamic || operand->rows.is_dynamic ||
      operand->columns.is_dynamic) {
    return true;
  }
  if (operand->blocks.static_value < 0 || operand->rows.static_value < 0 ||
      operand->columns.static_value < 0) {
    return false;
  }

  const uint64_t block_count = (uint64_t)operand->blocks.static_value;
  const uint64_t row_count = (uint64_t)operand->rows.static_value;
  const uint64_t column_count = (uint64_t)operand->columns.static_value;
  uint64_t required_scale_count = 0;
  switch (operand->schema.scale_topology) {
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_TENSOR_GLOBAL:
      required_scale_count = block_count && row_count && column_count ? 1 : 0;
      break;
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_ROW:
      required_scale_count = block_count && column_count ? row_count : 0;
      break;
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_COLUMN:
      required_scale_count = block_count && row_count ? column_count : 0;
      break;
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_GROUP_1D:
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_BLOCK_1D: {
      uint64_t block_row_count = 0;
      uint64_t element_count = 0;
      if (!iree_checked_mul_u64(block_count, row_count, &block_row_count) ||
          !iree_checked_mul_u64(block_row_count, column_count,
                                &element_count)) {
        return false;
      }
      const uint64_t group_element_count =
          operand->schema.scale_group.element_count;
      required_scale_count = element_count / group_element_count +
                             (element_count % group_element_count != 0);
      break;
    }
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_BLOCK_2D: {
      const uint64_t group_rows = operand->schema.scale_group.shape[0];
      const uint64_t group_columns = operand->schema.scale_group.shape[1];
      const uint64_t scale_rows = (row_count + group_rows - 1) / group_rows;
      const uint64_t scale_columns =
          (column_count + group_columns - 1) / group_columns;
      uint64_t block_scale_rows = 0;
      if (!iree_checked_mul_u64(block_count, scale_rows, &block_scale_rows) ||
          !iree_checked_mul_u64(block_scale_rows, scale_columns,
                                &required_scale_count)) {
        return false;
      }
      break;
    }
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_NONE:
    default:
      return false;
  }

  const uint64_t packed_word_count =
      (uint64_t)loom_type_dim_static_size_at(value_type, 0);
  const uint64_t required_word_count =
      required_scale_count / LOOM_VECTOR_TO_SCALAR_E8M0_BYTES_PER_WORD +
      (required_scale_count % LOOM_VECTOR_TO_SCALAR_E8M0_BYTES_PER_WORD != 0);
  return required_word_count <= packed_word_count;
}

static bool loom_vector_to_scalar_encoded_logical_element_count_matches(
    const loom_vector_to_scalar_encoded_operand_t* operand) {
  if (operand->blocks.is_dynamic || operand->rows.is_dynamic ||
      operand->columns.is_dynamic) {
    return true;
  }
  if (operand->blocks.static_value < 0 || operand->rows.static_value < 0 ||
      operand->columns.static_value < 0) {
    return false;
  }
  const uint64_t block_count = (uint64_t)operand->blocks.static_value;
  const uint64_t row_count = (uint64_t)operand->rows.static_value;
  const uint64_t column_count = (uint64_t)operand->columns.static_value;
  if (column_count != 0 && row_count > UINT64_MAX / column_count) {
    return false;
  }
  uint64_t element_count = row_count * column_count;
  if (block_count != 0 && element_count > UINT64_MAX / block_count) {
    return false;
  }
  element_count *= block_count;
  return element_count <= UINT16_MAX &&
         operand->schema.payload_element_count == (uint16_t)element_count;
}

static bool loom_vector_to_scalar_encoded_physical_lane_type_matches(
    loom_value_fact_encoded_operand_schema_t schema,
    loom_type_t raw_lane_type) {
  loom_scalar_type_t expected_scalar_type = LOOM_SCALAR_TYPE_NONE;
  if (!loom_numeric_format_direct_scalar_type(schema.element_format,
                                              &expected_scalar_type)) {
    return false;
  }
  return loom_type_element_type(raw_lane_type) == expected_scalar_type;
}

static bool loom_vector_to_scalar_encoded_affine_is_supported(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_type_t result_type) {
  if (!loom_vector_to_scalar_encoded_schema_has_scale_affine(operand->schema)) {
    return true;
  }
  if (!loom_scalar_type_is_float(loom_type_element_type(result_type))) {
    return false;
  }

  if (loom_vector_to_scalar_encoded_schema_has_scale(operand->schema)) {
    const bool scale_is_supported =
        loom_vector_to_scalar_encoded_schema_uses_e8m0_scale(operand->schema)
            ? loom_vector_to_scalar_encoded_e8m0_auxiliary_is_supported(
                  state, operand, LOOM_ENCODING_AUXILIARY_KEY_SCALE)
            : loom_vector_to_scalar_encoded_auxiliary_matches_format(
                  state, operand, LOOM_ENCODING_AUXILIARY_KEY_SCALE,
                  operand->schema.scale_format, result_type);
    if (!scale_is_supported) {
      return false;
    }
  }

  switch (operand->schema.affine_policy) {
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_ZERO_POINT:
      return loom_vector_to_scalar_encoded_auxiliary_matches_format(
          state, operand, LOOM_ENCODING_AUXILIARY_KEY_ZERO_POINT,
          operand->schema.element_format, result_type);
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_MIN:
      return loom_vector_to_scalar_encoded_auxiliary_lane_cast_is_supported(
          state, operand, LOOM_ENCODING_AUXILIARY_KEY_MINIMUM, result_type);
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_BIAS:
      return loom_vector_to_scalar_encoded_auxiliary_lane_cast_is_supported(
          state, operand, LOOM_ENCODING_AUXILIARY_KEY_BIAS, result_type);
    case LOOM_VALUE_FACT_AFFINE_POLICY_NONE:
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_ONLY:
    default:
      return true;
  }
}

static bool loom_vector_to_scalar_encoded_codebook_is_supported(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_type_t result_type) {
  if (!loom_vector_to_scalar_encoded_schema_uses_codebook(operand->schema)) {
    return true;
  }
  loom_type_t table_lane_type = {0};
  if (!loom_vector_to_scalar_encoded_auxiliary_lane_type(
          state, operand, LOOM_ENCODING_AUXILIARY_KEY_CODEBOOK,
          &table_lane_type)) {
    return false;
  }
  return loom_scalar_type_is_float(loom_type_element_type(table_lane_type)) &&
         loom_scalar_type_is_float(loom_type_element_type(result_type)) &&
         loom_vector_to_scalar_numeric_lane_cast_is_supported(table_lane_type,
                                                              result_type);
}

static bool loom_vector_to_scalar_encode_rounding_is_supported(
    loom_value_fact_encoded_operand_schema_t schema) {
  const loom_value_fact_rounding_policy_flags_t supported =
      LOOM_VALUE_FACT_ROUNDING_POLICY_NEAREST_EVEN |
      LOOM_VALUE_FACT_ROUNDING_POLICY_FINITE_ONLY;
  if ((schema.rounding_policy & ~supported) != 0) {
    return false;
  }
  return !iree_any_bit_set(schema.rounding_policy,
                           LOOM_VALUE_FACT_ROUNDING_POLICY_FINITE_ONLY) ||
         loom_numeric_format_is_finite_only(schema.element_format);
}

static bool loom_vector_to_scalar_encode_float_format_is_exact(
    loom_value_fact_encoded_operand_schema_t schema,
    loom_type_t physical_lane_type) {
  const loom_numeric_format_info_t* info = NULL;
  if (!loom_numeric_format_info(schema.element_format, &info)) {
    return false;
  }
  if (info->kind != LOOM_NUMERIC_FORMAT_KIND_FLOAT) {
    return true;
  }
  return loom_numeric_format_from_scalar_type(loom_type_element_type(
             physical_lane_type)) == schema.element_format;
}

bool loom_vector_to_scalar_encoded_operand_is_supported(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand) {
  IREE_ASSERT_ARGUMENT(state);
  IREE_ASSERT_ARGUMENT(operand);
  return loom_vector_to_scalar_encoded_operand_rejection_bits(state, operand) ==
         LOOM_CONTRACT_REJECTION_NONE;
}

uint32_t loom_vector_to_scalar_encoded_operand_rejection_bits(
    const loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand) {
  IREE_ASSERT_ARGUMENT(state);
  IREE_ASSERT_ARGUMENT(operand);
  loom_contract_rejection_bits_t rejection_bits = LOOM_CONTRACT_REJECTION_NONE;
  if (!loom_vector_to_scalar_encoded_schema_is_supported(operand->schema)) {
    return LOOM_CONTRACT_REJECTION_SCHEMA;
  }
  if (!loom_vector_to_scalar_encoded_schema_auxiliary_is_supported(
          operand->schema, operand->auxiliary)) {
    rejection_bits |= LOOM_CONTRACT_REJECTION_AUXILIARY_OPERAND;
  }
  if (!loom_vector_to_scalar_encoded_logical_element_count_matches(operand)) {
    rejection_bits |= LOOM_CONTRACT_REJECTION_SHAPE;
  }
  if (!loom_vector_to_scalar_encoded_physical_lane_type_matches(
          operand->schema, operand->physical_lane_type)) {
    rejection_bits |= LOOM_CONTRACT_REJECTION_NUMERIC;
  }
  const loom_type_t arithmetic_lane_type =
      loom_vector_to_scalar_encoded_arithmetic_lane_type(operand);
  if (!loom_vector_to_scalar_encoded_affine_is_supported(
          state, operand, arithmetic_lane_type)) {
    rejection_bits |= LOOM_CONTRACT_REJECTION_NUMERIC |
                      LOOM_CONTRACT_REJECTION_AUXILIARY_OPERAND;
  }
  switch (operand->direction) {
    case LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_DECODE:
      if (!loom_vector_to_scalar_numeric_lane_cast_is_supported(
              operand->physical_lane_type, arithmetic_lane_type) ||
          !loom_vector_to_scalar_numeric_lane_cast_is_supported(
              arithmetic_lane_type, operand->logical_lane_type)) {
        rejection_bits |= LOOM_CONTRACT_REJECTION_NUMERIC;
      }
      if (!loom_vector_to_scalar_encoded_codebook_is_supported(
              state, operand, arithmetic_lane_type)) {
        rejection_bits |= LOOM_CONTRACT_REJECTION_NUMERIC |
                          LOOM_CONTRACT_REJECTION_AUXILIARY_OPERAND;
      }
      break;
    case LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_ENCODE:
      if (!loom_vector_to_scalar_numeric_lane_cast_is_supported(
              operand->logical_lane_type, arithmetic_lane_type) ||
          !loom_vector_to_scalar_numeric_lane_cast_is_supported(
              arithmetic_lane_type, operand->physical_lane_type) ||
          !loom_vector_to_scalar_encode_rounding_is_supported(
              operand->schema) ||
          !loom_vector_to_scalar_encode_float_format_is_exact(
              operand->schema, operand->physical_lane_type) ||
          loom_vector_to_scalar_encoded_schema_uses_codebook(operand->schema)) {
        rejection_bits |= LOOM_CONTRACT_REJECTION_NUMERIC;
      }
      break;
    default:
      return LOOM_CONTRACT_REJECTION_INVALID_REQUEST;
  }
  return rejection_bits;
}

//===----------------------------------------------------------------------===//
// Lane construction
//===----------------------------------------------------------------------===//

static iree_status_t loom_vector_to_scalar_cast_lane_to_index(
    loom_vector_to_scalar_state_t* state, loom_value_id_t input,
    loom_type_t input_type, loom_value_id_t* out_index) {
  loom_type_t index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  if (loom_type_equal(input_type, index_type)) {
    *out_index = input;
    return iree_ok_status();
  }
  loom_op_t* cast_op = NULL;
  IREE_RETURN_IF_ERROR(loom_index_cast_build(&state->rewriter->builder, input,
                                             input_type, index_type,
                                             state->location, &cast_op));
  *out_index = loom_index_cast_result(cast_op);
  return iree_ok_status();
}

static iree_status_t loom_vector_to_scalar_encoded_auxiliary_lane(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_encoding_auxiliary_key_t key, loom_vector_to_scalar_index_term_t index,
    loom_value_id_t* out_lane) {
  loom_value_id_t vector_value = operand->auxiliary.values[key];
  loom_vector_to_scalar_index_list_t indices = {0};
  IREE_RETURN_IF_ERROR(
      loom_vector_to_scalar_terms_to_index_list(state, &index, 1, &indices));
  return loom_vector_to_scalar_materialize_lane(state, vector_value, indices,
                                                out_lane);
}

static iree_status_t loom_vector_to_scalar_encoded_e8m0_scale_lane(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_vector_to_scalar_index_term_t scale_index,
    loom_value_id_t* out_scale) {
  loom_vector_to_scalar_index_term_t word_index = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
      state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_DIV, scale_index,
      loom_vector_to_scalar_static_term(
          LOOM_VECTOR_TO_SCALAR_E8M0_BYTES_PER_WORD),
      &word_index));
  loom_vector_to_scalar_index_term_t byte_index = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
      state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_REM, scale_index,
      loom_vector_to_scalar_static_term(
          LOOM_VECTOR_TO_SCALAR_E8M0_BYTES_PER_WORD),
      &byte_index));
  loom_vector_to_scalar_index_term_t byte_shift = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
      state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_MUL, byte_index,
      loom_vector_to_scalar_static_term(LOOM_VECTOR_TO_SCALAR_E8M0_BYTE_BITS),
      &byte_shift));

  const loom_value_id_t scale_vector =
      operand->auxiliary.values[LOOM_ENCODING_AUXILIARY_KEY_SCALE];
  const loom_type_t scale_vector_type =
      loom_module_value_type(state->rewriter->module, scale_vector);
  const loom_type_t i32_type = loom_type_scalar(LOOM_SCALAR_TYPE_I32);
  loom_value_id_t packed_word = LOOM_VALUE_ID_INVALID;
  if (!word_index.is_dynamic) {
    IREE_RETURN_IF_ERROR(
        loom_vector_to_scalar_materialize_cached_static_integer_lane(
            state, scale_vector, scale_vector_type, word_index.static_value,
            i32_type, LOOM_VECTOR_TO_SCALAR_INTEGER_EXTENSION_ZERO,
            &packed_word));
  } else {
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_auxiliary_lane(
        state, operand, LOOM_ENCODING_AUXILIARY_KEY_SCALE, word_index,
        &packed_word));
  }

  loom_value_id_t scale_byte = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_shift_term(
      state, LOOM_OP_SCALAR_SHRUI, packed_word, i32_type, byte_shift,
      &scale_byte));
  loom_value_id_t byte_mask = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, i32_type, state->location,
      LOOM_VECTOR_TO_SCALAR_E8M0_BYTE_MASK, &byte_mask));
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_binary(
      state, LOOM_OP_SCALAR_ANDI, scale_byte, byte_mask, i32_type,
      &scale_byte));

  loom_value_id_t exponent_bits = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_shift(
      state, LOOM_OP_SCALAR_SHLI, scale_byte, i32_type,
      LOOM_VECTOR_TO_SCALAR_E8M0_F32_EXPONENT_SHIFT, &exponent_bits));

  loom_value_id_t zero = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, i32_type, state->location, 0, &zero));
  loom_op_t* is_minimum_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scalar_cmpi_build(
      &state->rewriter->builder, LOOM_SCALAR_CMPI_PREDICATE_EQ, scale_byte,
      zero, state->location, &is_minimum_op));
  loom_value_id_t minimum_bits = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, i32_type, state->location,
      LOOM_VECTOR_TO_SCALAR_E8M0_MINIMUM_F32_BITS, &minimum_bits));
  loom_op_t* minimum_select_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_select_build(
      &state->rewriter->builder, loom_scalar_cmpi_result(is_minimum_op),
      minimum_bits, exponent_bits, i32_type, state->location,
      &minimum_select_op));
  loom_value_id_t scale_bits = loom_scf_select_result(minimum_select_op);

  loom_value_id_t nan_byte = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, i32_type, state->location,
      LOOM_VECTOR_TO_SCALAR_E8M0_BYTE_MASK, &nan_byte));
  loom_op_t* is_nan_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scalar_cmpi_build(
      &state->rewriter->builder, LOOM_SCALAR_CMPI_PREDICATE_EQ, scale_byte,
      nan_byte, state->location, &is_nan_op));
  loom_value_id_t quiet_nan_bits = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, i32_type, state->location,
      LOOM_VECTOR_TO_SCALAR_E8M0_QUIET_NAN_F32_BITS, &quiet_nan_bits));
  loom_op_t* nan_select_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_select_build(
      &state->rewriter->builder, loom_scalar_cmpi_result(is_nan_op),
      quiet_nan_bits, scale_bits, i32_type, state->location, &nan_select_op));

  const loom_type_t f32_type = loom_type_scalar(LOOM_SCALAR_TYPE_F32);
  loom_op_t* bitcast_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scalar_bitcast_build(
      &state->rewriter->builder, loom_scf_select_result(nan_select_op),
      i32_type, f32_type, state->location, &bitcast_op));
  *out_scale = loom_scalar_bitcast_result(bitcast_op);
  return iree_ok_status();
}

static iree_status_t loom_vector_to_scalar_encoded_ceil_div(
    loom_vector_to_scalar_state_t* state,
    loom_vector_to_scalar_index_term_t value, uint16_t divisor,
    loom_vector_to_scalar_index_term_t* out_result) {
  loom_vector_to_scalar_index_term_t adjusted = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
      state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_ADD, value,
      loom_vector_to_scalar_static_term((int64_t)divisor - 1), &adjusted));
  return loom_vector_to_scalar_build_term_binary(
      state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_DIV, adjusted,
      loom_vector_to_scalar_static_term(divisor), out_result);
}

static iree_status_t loom_vector_to_scalar_encoded_scale_index(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_vector_to_scalar_index_term_t block,
    loom_vector_to_scalar_index_term_t row,
    loom_vector_to_scalar_index_term_t column,
    loom_vector_to_scalar_index_term_t ordinal,
    loom_vector_to_scalar_index_term_t* out_index) {
  switch (operand->schema.scale_topology) {
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_TENSOR_GLOBAL:
      *out_index = loom_vector_to_scalar_static_term(0);
      return iree_ok_status();
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_ROW:
      *out_index = row;
      return iree_ok_status();
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_COLUMN:
      *out_index = column;
      return iree_ok_status();
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_GROUP_1D:
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_BLOCK_1D:
      return loom_vector_to_scalar_build_term_binary(
          state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_DIV, ordinal,
          loom_vector_to_scalar_static_term(
              operand->schema.scale_group.element_count),
          out_index);
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_BLOCK_2D: {
      const uint16_t scale_block_row_extent =
          operand->schema.scale_group.shape[0];
      const uint16_t scale_block_column_extent =
          operand->schema.scale_group.shape[1];
      loom_vector_to_scalar_index_term_t scale_column_count = {0};
      IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_ceil_div(
          state, operand->columns, scale_block_column_extent,
          &scale_column_count));
      loom_vector_to_scalar_index_term_t scale_row = {0};
      IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
          state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_DIV, row,
          loom_vector_to_scalar_static_term(scale_block_row_extent),
          &scale_row));
      loom_vector_to_scalar_index_term_t scale_column = {0};
      IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
          state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_DIV, column,
          loom_vector_to_scalar_static_term(scale_block_column_extent),
          &scale_column));

      loom_vector_to_scalar_index_term_t scale_row_offset = {0};
      IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
          state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_MUL, scale_row,
          scale_column_count, &scale_row_offset));
      loom_vector_to_scalar_index_term_t scale_index = scale_row_offset;
      if (block.is_dynamic || block.static_value != 0) {
        loom_vector_to_scalar_index_term_t scale_row_count = {0};
        IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_ceil_div(
            state, operand->rows, scale_block_row_extent, &scale_row_count));
        loom_vector_to_scalar_index_term_t scales_per_block = {0};
        IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
            state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_MUL, scale_row_count,
            scale_column_count, &scales_per_block));
        loom_vector_to_scalar_index_term_t scale_block_offset = {0};
        IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
            state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_MUL, block,
            scales_per_block, &scale_block_offset));
        IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_term_binary(
            state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_ADD, scale_block_offset,
            scale_row_offset, &scale_index));
      }
      return loom_vector_to_scalar_build_term_binary(
          state, LOOM_VECTOR_TO_SCALAR_INDEX_BINARY_ADD, scale_index,
          scale_column, out_index);
    }
    case LOOM_VALUE_FACT_SCALE_TOPOLOGY_NONE:
    default:
      *out_index = loom_vector_to_scalar_static_term(0);
      return iree_ok_status();
  }
}

static iree_status_t loom_vector_to_scalar_encoded_codebook_lane(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_value_id_t raw_lane, loom_type_t raw_lane_type,
    loom_type_t result_type, loom_value_id_t* out_lane) {
  loom_value_id_t table_index = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_cast_lane_to_index(
      state, raw_lane, raw_lane_type, &table_index));

  loom_vector_to_scalar_index_list_t table_indices = {
      .dynamic_indices = &table_index,
      .rank = 1,
  };
  loom_value_id_t codebook =
      operand->auxiliary.values[LOOM_ENCODING_AUXILIARY_KEY_CODEBOOK];
  loom_value_id_t table_lane = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_materialize_lane(
      state, codebook, table_indices, &table_lane));

  loom_type_t codebook_type =
      loom_module_value_type(state->rewriter->module, codebook);
  loom_type_t table_lane_type = loom_vector_to_scalar_lane_type(codebook_type);
  return loom_vector_to_scalar_cast_numeric_lane(
      state, table_lane, table_lane_type, result_type,
      /*unsigned_input=*/false, out_lane);
}

static iree_status_t loom_vector_to_scalar_encoded_raw_value_lane(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_value_id_t raw_lane, loom_type_t raw_lane_type,
    loom_type_t result_type, loom_value_id_t* out_lane) {
  if (loom_vector_to_scalar_encoded_schema_uses_codebook(operand->schema)) {
    return loom_vector_to_scalar_encoded_codebook_lane(
        state, operand, raw_lane, raw_lane_type, result_type, out_lane);
  }
  return loom_vector_to_scalar_cast_numeric_lane(
      state, raw_lane, raw_lane_type, result_type,
      loom_numeric_format_uses_unsigned_integer_semantics(
          operand->schema.element_format),
      out_lane);
}

static iree_status_t loom_vector_to_scalar_encoded_affine_operand_lane(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_encoding_auxiliary_key_t key, loom_vector_to_scalar_index_term_t index,
    loom_type_t result_type, bool unsigned_input, loom_value_id_t* out_lane) {
  if (key == LOOM_ENCODING_AUXILIARY_KEY_SCALE &&
      loom_vector_to_scalar_encoded_schema_uses_e8m0_scale(operand->schema)) {
    loom_value_id_t scale = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_e8m0_scale_lane(
        state, operand, index, &scale));
    return loom_vector_to_scalar_cast_numeric_lane(
        state, scale, loom_type_scalar(LOOM_SCALAR_TYPE_F32), result_type,
        /*unsigned_input=*/false, out_lane);
  }

  loom_value_id_t lane = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_auxiliary_lane(
      state, operand, key, index, &lane));
  loom_value_id_t vector_value = operand->auxiliary.values[key];
  loom_type_t vector_type =
      loom_module_value_type(state->rewriter->module, vector_value);
  loom_type_t lane_type = loom_vector_to_scalar_lane_type(vector_type);
  return loom_vector_to_scalar_cast_numeric_lane(
      state, lane, lane_type, result_type, unsigned_input, out_lane);
}

static loom_encoding_auxiliary_key_t loom_vector_to_scalar_encoded_offset_key(
    loom_value_fact_affine_policy_flags_t affine_policy) {
  switch (affine_policy) {
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_MIN:
      return LOOM_ENCODING_AUXILIARY_KEY_MINIMUM;
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_BIAS:
      return LOOM_ENCODING_AUXILIARY_KEY_BIAS;
    case LOOM_VALUE_FACT_AFFINE_POLICY_NONE:
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_ONLY:
    case LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_ZERO_POINT:
    default:
      return LOOM_ENCODING_AUXILIARY_KEY_COUNT_;
  }
}

static iree_status_t loom_vector_to_scalar_encoded_apply_decode_scale(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_vector_to_scalar_index_term_t scale_index, loom_type_t result_type,
    loom_value_id_t input, loom_value_id_t* out_lane) {
  if (!loom_vector_to_scalar_encoded_schema_has_scale(operand->schema)) {
    *out_lane = input;
    return iree_ok_status();
  }
  loom_value_id_t scale = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_affine_operand_lane(
      state, operand, LOOM_ENCODING_AUXILIARY_KEY_SCALE, scale_index,
      result_type, /*unsigned_input=*/false, &scale));
  return loom_vector_to_scalar_build_scalar_binary(
      state, LOOM_OP_SCALAR_MULF, input, scale, result_type, out_lane);
}

static iree_status_t loom_vector_to_scalar_encoded_apply_decode_affine(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_vector_to_scalar_index_term_t scale_index, loom_type_t result_type,
    loom_value_id_t input, loom_value_id_t* out_lane) {
  loom_value_id_t value = input;
  if (operand->schema.affine_policy ==
      LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_ZERO_POINT) {
    loom_value_id_t zero_point = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_affine_operand_lane(
        state, operand, LOOM_ENCODING_AUXILIARY_KEY_ZERO_POINT, scale_index,
        result_type,
        loom_numeric_format_uses_unsigned_integer_semantics(
            operand->schema.element_format),
        &zero_point));
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_binary(
        state, LOOM_OP_SCALAR_SUBF, value, zero_point, result_type, &value));
  }

  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_apply_decode_scale(
      state, operand, scale_index, result_type, value, &value));

  const loom_encoding_auxiliary_key_t offset_key =
      loom_vector_to_scalar_encoded_offset_key(operand->schema.affine_policy);
  if (offset_key == LOOM_ENCODING_AUXILIARY_KEY_COUNT_) {
    *out_lane = value;
    return iree_ok_status();
  }

  loom_value_id_t offset = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_affine_operand_lane(
      state, operand, offset_key, scale_index, result_type,
      /*unsigned_input=*/false, &offset));
  return loom_vector_to_scalar_build_scalar_binary(
      state, LOOM_OP_SCALAR_ADDF, value, offset, result_type, out_lane);
}

static iree_status_t loom_vector_to_scalar_encoded_apply_encode_affine(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_vector_to_scalar_index_term_t scale_index, loom_value_id_t input,
    loom_value_id_t* out_lane) {
  const loom_type_t arithmetic_lane_type =
      loom_vector_to_scalar_encoded_arithmetic_lane_type(operand);
  loom_value_id_t value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_cast_numeric_lane(
      state, input, operand->logical_lane_type, arithmetic_lane_type,
      /*unsigned_input=*/false, &value));

  const loom_encoding_auxiliary_key_t offset_key =
      loom_vector_to_scalar_encoded_offset_key(operand->schema.affine_policy);
  if (offset_key != LOOM_ENCODING_AUXILIARY_KEY_COUNT_) {
    loom_value_id_t offset = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_affine_operand_lane(
        state, operand, offset_key, scale_index, arithmetic_lane_type,
        /*unsigned_input=*/false, &offset));
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_binary(
        state, LOOM_OP_SCALAR_SUBF, value, offset, arithmetic_lane_type,
        &value));
  }

  if (loom_vector_to_scalar_encoded_schema_has_scale(operand->schema)) {
    loom_value_id_t scale = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_affine_operand_lane(
        state, operand, LOOM_ENCODING_AUXILIARY_KEY_SCALE, scale_index,
        arithmetic_lane_type, /*unsigned_input=*/false, &scale));
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_binary(
        state, LOOM_OP_SCALAR_DIVF, value, scale, arithmetic_lane_type,
        &value));
  }

  if (operand->schema.affine_policy ==
      LOOM_VALUE_FACT_AFFINE_POLICY_SCALE_PLUS_ZERO_POINT) {
    loom_value_id_t zero_point = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_affine_operand_lane(
        state, operand, LOOM_ENCODING_AUXILIARY_KEY_ZERO_POINT, scale_index,
        arithmetic_lane_type,
        loom_numeric_format_uses_unsigned_integer_semantics(
            operand->schema.element_format),
        &zero_point));
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_binary(
        state, LOOM_OP_SCALAR_ADDF, value, zero_point, arithmetic_lane_type,
        &value));
  }

  return loom_vector_to_scalar_cast_numeric_lane(
      state, value, arithmetic_lane_type, operand->physical_lane_type,
      /*unsigned_input=*/false, out_lane);
}

iree_status_t loom_vector_to_scalar_build_decoded_lane(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_value_id_t physical_lane, loom_vector_to_scalar_index_term_t block,
    loom_vector_to_scalar_index_term_t row,
    loom_vector_to_scalar_index_term_t column,
    loom_vector_to_scalar_index_term_t ordinal, loom_value_id_t* out_lane) {
  IREE_ASSERT_ARGUMENT(state);
  IREE_ASSERT_ARGUMENT(operand);
  IREE_ASSERT_ARGUMENT(out_lane);
  IREE_ASSERT(
      loom_vector_to_scalar_encoded_operand_is_supported(state, operand),
      "unsupported encoded operand reached scalar reference builder");
  IREE_ASSERT_EQ(operand->direction,
                 LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_DECODE);

  const loom_type_t arithmetic_lane_type =
      loom_vector_to_scalar_encoded_arithmetic_lane_type(operand);
  loom_value_id_t decoded = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_raw_value_lane(
      state, operand, physical_lane, operand->physical_lane_type,
      arithmetic_lane_type, &decoded));

  loom_vector_to_scalar_index_term_t scale_index = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_scale_index(
      state, operand, block, row, column, ordinal, &scale_index));
  loom_value_id_t affine_result = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_apply_decode_affine(
      state, operand, scale_index, arithmetic_lane_type, decoded,
      &affine_result));
  return loom_vector_to_scalar_cast_numeric_lane(
      state, affine_result, arithmetic_lane_type, operand->logical_lane_type,
      /*unsigned_input=*/false, out_lane);
}

static iree_status_t loom_vector_to_scalar_build_encoded_lane(
    loom_vector_to_scalar_state_t* state,
    const loom_vector_to_scalar_encoded_operand_t* operand,
    loom_value_id_t logical_lane, loom_vector_to_scalar_index_term_t block,
    loom_vector_to_scalar_index_term_t row,
    loom_vector_to_scalar_index_term_t column,
    loom_vector_to_scalar_index_term_t ordinal, loom_value_id_t* out_lane) {
  IREE_ASSERT(
      loom_vector_to_scalar_encoded_operand_is_supported(state, operand),
      "unsupported encoded operand reached scalar reference builder");
  IREE_ASSERT_EQ(operand->direction,
                 LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_ENCODE);

  loom_vector_to_scalar_index_term_t scale_index = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_scale_index(
      state, operand, block, row, column, ordinal, &scale_index));
  return loom_vector_to_scalar_encoded_apply_encode_affine(
      state, operand, scale_index, logical_lane, out_lane);
}

//===----------------------------------------------------------------------===//
// Standalone vector.encode/vector.decode
//===----------------------------------------------------------------------===//

static uint32_t loom_vector_to_scalar_standalone_encoded_operand(
    loom_vector_to_scalar_state_t* state,
    loom_vector_to_scalar_encoding_direction_t direction,
    loom_vector_to_scalar_encoded_operand_t* out_operand) {
  if (!state->rewriter->fact_table) {
    return LOOM_CONTRACT_REJECTION_SCHEMA;
  }

  loom_value_id_t schema_value = LOOM_VALUE_ID_INVALID;
  loom_value_id_t logical_value = LOOM_VALUE_ID_INVALID;
  loom_type_t logical_type = {0};
  loom_type_t physical_type = {0};
  loom_value_slice_t auxiliary_values = {0};
  loom_named_attr_slice_t auxiliary_names = {0};
  switch (direction) {
    case LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_DECODE: {
      if (!loom_vector_decode_isa(state->op)) {
        return LOOM_CONTRACT_REJECTION_INVALID_REQUEST;
      }
      const loom_value_id_t physical_value =
          loom_vector_decode_payload(state->op);
      schema_value = loom_vector_decode_schema(state->op);
      logical_type = state->vector_type;
      physical_type =
          loom_module_value_type(state->rewriter->module, physical_value);
      auxiliary_values = loom_vector_decode_auxiliary(state->op);
      auxiliary_names = loom_vector_decode_auxiliary_names(state->op);
      break;
    }
    case LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_ENCODE:
      if (!loom_vector_encode_isa(state->op)) {
        return LOOM_CONTRACT_REJECTION_INVALID_REQUEST;
      }
      logical_value = loom_vector_encode_source(state->op);
      schema_value = loom_vector_encode_schema(state->op);
      logical_type =
          loom_module_value_type(state->rewriter->module, logical_value);
      physical_type = state->vector_type;
      auxiliary_values = loom_vector_encode_auxiliary(state->op);
      auxiliary_names = loom_vector_encode_auxiliary_names(state->op);
      break;
    default:
      return LOOM_CONTRACT_REJECTION_INVALID_REQUEST;
  }

  if (!loom_type_is_vector(logical_type) ||
      !loom_type_is_vector(physical_type)) {
    return LOOM_CONTRACT_REJECTION_SHAPE;
  }

  const uint8_t rank = loom_type_rank(logical_type);
  if ((rank != 1 && rank != 2) ||
      !loom_type_shape_equals(physical_type, logical_type)) {
    return LOOM_CONTRACT_REJECTION_SHAPE;
  }

  loom_value_fact_encoding_summary_t summary = {0};
  if (!loom_value_facts_query_encoding_summary(
          &state->rewriter->fact_table->context,
          loom_rewriter_value_facts(state->rewriter, schema_value), &summary)) {
    return LOOM_CONTRACT_REJECTION_SCHEMA;
  }

  loom_encoding_auxiliary_view_t auxiliary = {0};
  if (!loom_encoding_auxiliary_view_resolve(state->rewriter->module,
                                            auxiliary_values, auxiliary_names,
                                            &auxiliary, NULL)) {
    return LOOM_CONTRACT_REJECTION_AUXILIARY_OPERAND;
  }

  *out_operand = (loom_vector_to_scalar_encoded_operand_t){
      .schema = summary.storage_schema.encoded_operand,
      .auxiliary = auxiliary,
      .blocks = loom_vector_to_scalar_static_term(1),
      .rows = rank == 1 ? loom_vector_to_scalar_static_term(1)
                        : loom_vector_to_scalar_dim_bound_term(state,
                                                               logical_type, 0),
      .columns = loom_vector_to_scalar_dim_bound_term(state, logical_type,
                                                      (uint8_t)(rank - 1)),
      .logical_lane_type = loom_vector_to_scalar_lane_type(logical_type),
      .physical_lane_type = loom_vector_to_scalar_lane_type(physical_type),
      .direction = direction,
  };
  return loom_vector_to_scalar_encoded_operand_rejection_bits(state,
                                                              out_operand);
}

uint32_t loom_vector_to_scalar_encoding_rejection_bits(
    loom_vector_to_scalar_state_t* state) {
  loom_vector_to_scalar_encoded_operand_t operand = {0};
  if (loom_vector_decode_isa(state->op)) {
    return loom_vector_to_scalar_standalone_encoded_operand(
        state, LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_DECODE, &operand);
  }
  if (loom_vector_encode_isa(state->op)) {
    return loom_vector_to_scalar_standalone_encoded_operand(
        state, LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_ENCODE, &operand);
  }
  return LOOM_CONTRACT_REJECTION_INVALID_REQUEST;
}

static iree_status_t loom_vector_to_scalar_encoded_coordinates(
    loom_vector_to_scalar_state_t* state,
    loom_vector_to_scalar_index_list_t indices,
    loom_vector_to_scalar_index_term_t ordinal,
    loom_vector_to_scalar_index_term_t* out_row,
    loom_vector_to_scalar_index_term_t* out_column) {
  switch (indices.rank) {
    case 1:
      *out_row = loom_vector_to_scalar_static_term(0);
      *out_column = ordinal;
      return iree_ok_status();
    case 2:
      *out_row = loom_vector_to_scalar_lane_term(state, indices, 0);
      *out_column = loom_vector_to_scalar_lane_term(state, indices, 1);
      return iree_ok_status();
    default:
      IREE_ASSERT_UNREACHABLE(
          "unsupported encoded vector rank reached scalar reference builder");
      IREE_BUILTIN_UNREACHABLE();
  }
}

iree_status_t loom_vector_to_scalar_build_decode_lane(
    loom_vector_to_scalar_state_t* state,
    loom_vector_to_scalar_index_list_t indices, loom_value_id_t* out_lane) {
  loom_vector_to_scalar_encoded_operand_t operand = {0};
  if (loom_vector_to_scalar_standalone_encoded_operand(
          state, LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_DECODE, &operand) !=
      LOOM_CONTRACT_REJECTION_NONE) {
    IREE_ASSERT_UNREACHABLE(
        "unsupported vector.decode reached scalar reference builder");
    IREE_BUILTIN_UNREACHABLE();
  }

  const loom_value_id_t payload = loom_vector_decode_payload(state->op);
  loom_value_id_t raw_lane = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_materialize_lane(
      state, payload, indices, &raw_lane));

  loom_vector_to_scalar_index_term_t ordinal = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_linear_ordinal_term(
      state, state->vector_type, indices, &ordinal));
  loom_vector_to_scalar_index_term_t row = {0};
  loom_vector_to_scalar_index_term_t column = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_coordinates(
      state, indices, ordinal, &row, &column));
  return loom_vector_to_scalar_build_decoded_lane(
      state, &operand, raw_lane, loom_vector_to_scalar_static_term(0), row,
      column, ordinal, out_lane);
}

iree_status_t loom_vector_to_scalar_build_encode_lane(
    loom_vector_to_scalar_state_t* state,
    loom_vector_to_scalar_index_list_t indices, loom_value_id_t* out_lane) {
  loom_vector_to_scalar_encoded_operand_t operand = {0};
  if (loom_vector_to_scalar_standalone_encoded_operand(
          state, LOOM_VECTOR_TO_SCALAR_ENCODING_DIRECTION_ENCODE, &operand) !=
      LOOM_CONTRACT_REJECTION_NONE) {
    IREE_ASSERT_UNREACHABLE(
        "unsupported vector.encode reached scalar reference builder");
    IREE_BUILTIN_UNREACHABLE();
  }

  loom_value_id_t logical_lane = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_materialize_lane(
      state, loom_vector_encode_source(state->op), indices, &logical_lane));

  loom_vector_to_scalar_index_term_t ordinal = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_linear_ordinal_term(
      state, state->vector_type, indices, &ordinal));
  loom_vector_to_scalar_index_term_t row = {0};
  loom_vector_to_scalar_index_term_t column = {0};
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_encoded_coordinates(
      state, indices, ordinal, &row, &column));
  return loom_vector_to_scalar_build_encoded_lane(
      state, &operand, logical_lane, loom_vector_to_scalar_static_term(0), row,
      column, ordinal, out_lane);
}
