// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/legalization_float.h"

#include "loom/ir/module.h"
#include "loom/ops/vector/ops.h"
#include "loom/target/arch/amd/xdna/aie2p/descriptors/core_descriptors.h"
#include "loom/util/numeric_format.h"

typedef enum loom_aie2p_float_compare_relation_e {
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_EQ = 0,
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_GT,
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_GE,
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_LT,
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_LE,
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_NE,
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_ORDERED,
  LOOM_AIE2P_FLOAT_COMPARE_RELATION_UNORDERED,
} loom_aie2p_float_compare_relation_t;

typedef enum loom_aie2p_float_compare_nan_policy_e {
  LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR = 0,
  LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET,
} loom_aie2p_float_compare_nan_policy_t;

typedef struct loom_aie2p_float_compare_plan_t {
  // Numeric relation evaluated after mapping non-NaN values to integer keys.
  uint8_t relation;
  // Result behavior when either operand is a NaN.
  uint8_t nan_policy;
} loom_aie2p_float_compare_plan_t;

static const loom_aie2p_float_compare_plan_t kAie2pFloatComparePlans[] = {
    [LOOM_VECTOR_CMPF_PREDICATE_OEQ] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_EQ,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR},
    [LOOM_VECTOR_CMPF_PREDICATE_OGT] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_GT,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR},
    [LOOM_VECTOR_CMPF_PREDICATE_OGE] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_GE,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR},
    [LOOM_VECTOR_CMPF_PREDICATE_OLT] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_LT,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR},
    [LOOM_VECTOR_CMPF_PREDICATE_OLE] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_LE,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR},
    [LOOM_VECTOR_CMPF_PREDICATE_ONE] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_NE,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR},
    [LOOM_VECTOR_CMPF_PREDICATE_ORD] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_ORDERED,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_CLEAR},
    [LOOM_VECTOR_CMPF_PREDICATE_UEQ] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_EQ,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET},
    [LOOM_VECTOR_CMPF_PREDICATE_UGT] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_GT,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET},
    [LOOM_VECTOR_CMPF_PREDICATE_UGE] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_GE,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET},
    [LOOM_VECTOR_CMPF_PREDICATE_ULT] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_LT,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET},
    [LOOM_VECTOR_CMPF_PREDICATE_ULE] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_LE,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET},
    [LOOM_VECTOR_CMPF_PREDICATE_UNE] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_NE,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET},
    [LOOM_VECTOR_CMPF_PREDICATE_UNO] =
        {LOOM_AIE2P_FLOAT_COMPARE_RELATION_UNORDERED,
         LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET},
};
static_assert(IREE_ARRAYSIZE(kAie2pFloatComparePlans) ==
                  LOOM_VECTOR_CMPF_PREDICATE_COUNT_,
              "float comparison plan table out of sync");

static const uint8_t kAie2pFloatCompareIntegerPredicates[] = {
    [LOOM_AIE2P_FLOAT_COMPARE_RELATION_EQ] = LOOM_VECTOR_CMPI_PREDICATE_EQ,
    [LOOM_AIE2P_FLOAT_COMPARE_RELATION_GT] = LOOM_VECTOR_CMPI_PREDICATE_UGT,
    [LOOM_AIE2P_FLOAT_COMPARE_RELATION_GE] = LOOM_VECTOR_CMPI_PREDICATE_UGE,
    [LOOM_AIE2P_FLOAT_COMPARE_RELATION_LT] = LOOM_VECTOR_CMPI_PREDICATE_ULT,
    [LOOM_AIE2P_FLOAT_COMPARE_RELATION_LE] = LOOM_VECTOR_CMPI_PREDICATE_ULE,
    [LOOM_AIE2P_FLOAT_COMPARE_RELATION_NE] = LOOM_VECTOR_CMPI_PREDICATE_NE,
};
static_assert(IREE_ARRAYSIZE(kAie2pFloatCompareIntegerPredicates) ==
                  LOOM_AIE2P_FLOAT_COMPARE_RELATION_NE + 1,
              "float comparison relation table out of sync");

typedef struct loom_aie2p_float_ordering_context_t {
  // Builder receiving the integer-vector ordering operations.
  loom_builder_t* builder;
  // Original floating-point vector type.
  loom_type_t float_type;
  // Same-shape integer vector type carrying the encoded bits.
  loom_type_t integer_type;
  // Same-shape predicate vector type used by comparisons.
  loom_type_t predicate_type;
  // Encoded floating-point layout for the element type.
  loom_numeric_float_encoding_t encoding;
  // Source location copied to generated operations.
  loom_location_id_t location;
} loom_aie2p_float_ordering_context_t;

typedef struct loom_aie2p_float_ordering_value_t {
  // Original floating-point encoding carried in a same-width integer vector.
  loom_value_id_t bits;
  // Encoded magnitude with the sign bit removed.
  loom_value_id_t magnitude;
  // Monotonic unsigned ordering key for non-NaN values.
  loom_value_id_t key;
  // Per-lane predicate identifying every NaN encoding.
  loom_value_id_t is_nan;
} loom_aie2p_float_ordering_value_t;

// Floating semantic recipes are expressed in the same 512-bit packets used by
// AIE2P's integer-vector contract. Building the recipe at its authored
// 1024-bit carrier width would expose generated wide integer operations to the
// reference scalarizer before component packetization gets another turn.
#define LOOM_AIE2P_FLOAT_ORDERING_PACKET_BIT_COUNT 512u

static loom_type_t loom_aie2p_vector_type_with_element(
    loom_type_t source_type, loom_scalar_type_t element_type) {
  source_type.header = loom_type_make_header(
      loom_type_kind(source_type), element_type, loom_type_rank(source_type),
      loom_type_flags(source_type));
  return source_type;
}

static loom_type_t loom_aie2p_linear_vector_type(loom_type_t source_type,
                                                 uint32_t lane_count) {
  return loom_type_shaped_1d(
      LOOM_TYPE_VECTOR, loom_type_element_type(source_type),
      loom_dim_pack_static(lane_count),
      loom_type_rank(source_type) == 1 ? source_type.encoding_id : 0);
}

static iree_status_t loom_aie2p_float_ordering_linearize_value(
    loom_builder_t* builder, loom_value_id_t value, loom_type_t source_type,
    uint32_t lane_count, loom_location_id_t location,
    loom_value_id_t* out_value) {
  const loom_type_t linear_type =
      loom_aie2p_linear_vector_type(source_type, lane_count);
  if (loom_type_equal(source_type, linear_type)) {
    *out_value = value;
    return iree_ok_status();
  }
  loom_op_t* bitcast_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_bitcast_build(
      builder, value, source_type, linear_type, location, &bitcast_op));
  *out_value = loom_vector_bitcast_result(bitcast_op);
  return iree_ok_status();
}

static iree_status_t loom_aie2p_float_ordering_slice_value(
    loom_builder_t* builder, loom_value_id_t value, loom_type_t source_type,
    uint32_t lane_offset, uint32_t lane_count, loom_location_id_t location,
    loom_value_id_t* out_value) {
  const int64_t static_offset = lane_offset;
  const loom_type_t packet_type =
      loom_aie2p_linear_vector_type(source_type, lane_count);
  loom_op_t* slice_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_slice_build(
      builder, value, /*offsets=*/NULL, /*offsets_count=*/0, &static_offset,
      /*static_offsets_count=*/1, packet_type, location, &slice_op));
  *out_value = loom_vector_slice_result(slice_op);
  return iree_ok_status();
}

static void loom_aie2p_float_ordering_initialize(
    loom_builder_t* builder, loom_type_t float_type,
    loom_numeric_float_encoding_t encoding, loom_location_id_t location,
    loom_aie2p_float_ordering_context_t* out_ordering) {
  *out_ordering = (loom_aie2p_float_ordering_context_t){
      .builder = builder,
      .float_type = float_type,
      .integer_type = loom_aie2p_vector_type_with_element(
          float_type, encoding.integer_type),
      .predicate_type =
          loom_aie2p_vector_type_with_element(float_type, LOOM_SCALAR_TYPE_I1),
      .encoding = encoding,
      .location = location,
  };
}

static iree_status_t loom_aie2p_float_ordering_build_constant(
    loom_aie2p_float_ordering_context_t* ordering, int64_t value,
    loom_value_id_t* out_value) {
  loom_op_t* constant_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_constant_build(
      ordering->builder, loom_attr_i64(value), ordering->integer_type,
      ordering->location, &constant_op));
  *out_value = loom_vector_constant_result(constant_op);
  return iree_ok_status();
}

static iree_status_t loom_aie2p_float_ordering_build_compare(
    loom_aie2p_float_ordering_context_t* ordering, uint8_t predicate,
    loom_value_id_t lhs, loom_value_id_t rhs, loom_value_id_t* out_value) {
  loom_op_t* compare_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_cmpi_build(
      ordering->builder, predicate, lhs, rhs, ordering->integer_type,
      ordering->predicate_type, ordering->location, &compare_op));
  *out_value = loom_vector_cmpi_result(compare_op);
  return iree_ok_status();
}

static iree_status_t loom_aie2p_float_ordering_build_value(
    loom_aie2p_float_ordering_context_t* ordering, loom_value_id_t value,
    loom_value_id_t magnitude_mask,
    loom_aie2p_float_ordering_value_t* out_value) {
  loom_op_t* bitcast_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_bitcast_build(
      ordering->builder, value, ordering->float_type, ordering->integer_type,
      ordering->location, &bitcast_op));
  const loom_value_id_t bits = loom_vector_bitcast_result(bitcast_op);

  loom_op_t* magnitude_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_andi_build(
      ordering->builder, bits, magnitude_mask, ordering->integer_type,
      ordering->location, &magnitude_op));
  const loom_value_id_t magnitude = loom_vector_andi_result(magnitude_op);

  *out_value = (loom_aie2p_float_ordering_value_t){
      .bits = bits,
      .magnitude = magnitude,
  };
  return iree_ok_status();
}

static iree_status_t loom_aie2p_float_ordering_build_nan(
    loom_aie2p_float_ordering_context_t* ordering,
    loom_value_id_t special_magnitude,
    loom_aie2p_float_ordering_value_t* value) {
  const loom_vector_cmpi_predicate_t nan_predicate =
      ordering->encoding.special_layout ==
              LOOM_NUMERIC_FLOAT_SPECIAL_LAYOUT_IEEE
          ? LOOM_VECTOR_CMPI_PREDICATE_UGT
          : LOOM_VECTOR_CMPI_PREDICATE_EQ;
  return loom_aie2p_float_ordering_build_compare(
      ordering, nan_predicate, value->magnitude, special_magnitude,
      &value->is_nan);
}

static iree_status_t loom_aie2p_float_ordering_build_key(
    loom_aie2p_float_ordering_context_t* ordering, loom_value_id_t zero,
    loom_value_id_t sign_mask, loom_value_id_t all_ones,
    loom_aie2p_float_ordering_value_t* value) {
  // AIE2P has no packed i64 add/sub contract. Form the equivalent order key
  // with bitwise operations; the caller normalizes signed zero afterward.
  if (ordering->encoding.integer_type == LOOM_SCALAR_TYPE_I64) {
    loom_value_id_t is_negative = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_compare(
        ordering, LOOM_VECTOR_CMPI_PREDICATE_SLT, value->bits, zero,
        &is_negative));
    loom_op_t* adjustment_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_select_build(
        ordering->builder, is_negative, all_ones, sign_mask,
        ordering->integer_type, ordering->location, &adjustment_op));
    loom_op_t* key_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_xori_build(
        ordering->builder, value->bits,
        loom_vector_select_result(adjustment_op), ordering->integer_type,
        ordering->location, &key_op));
    value->key = loom_vector_xori_result(key_op);
    return iree_ok_status();
  }

  loom_value_id_t is_zero = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_compare(
      ordering, LOOM_VECTOR_CMPI_PREDICATE_EQ, value->magnitude, zero,
      &is_zero));
  loom_op_t* canonical_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_select_build(
      ordering->builder, is_zero, zero, value->bits, ordering->integer_type,
      ordering->location, &canonical_op));
  const loom_value_id_t canonical_bits =
      loom_vector_select_result(canonical_op);

  loom_value_id_t is_negative = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_compare(
      ordering, LOOM_VECTOR_CMPI_PREDICATE_SLT, canonical_bits, zero,
      &is_negative));
  loom_op_t* positive_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_addi_build(
      ordering->builder, /*overflow=*/0, canonical_bits, sign_mask,
      ordering->integer_type, ordering->location, &positive_op));
  loom_op_t* negative_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_subi_build(
      ordering->builder, /*overflow=*/0, all_ones, canonical_bits,
      ordering->integer_type, ordering->location, &negative_op));
  loom_op_t* key_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_select_build(
      ordering->builder, is_negative, loom_vector_subi_result(negative_op),
      loom_vector_addi_result(positive_op), ordering->integer_type,
      ordering->location, &key_op));
  value->key = loom_vector_select_result(key_op);
  return iree_ok_status();
}

static iree_status_t loom_aie2p_float_ordering_normalize_zero(
    loom_aie2p_float_ordering_context_t* ordering, loom_value_id_t zero,
    loom_value_id_t sign_mask, loom_aie2p_float_ordering_value_t* value) {
  loom_value_id_t is_zero = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_compare(
      ordering, LOOM_VECTOR_CMPI_PREDICATE_EQ, value->magnitude, zero,
      &is_zero));
  loom_op_t* key_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_select_build(
      ordering->builder, is_zero, sign_mask, value->key, ordering->integer_type,
      ordering->location, &key_op));
  value->key = loom_vector_select_result(key_op);
  return iree_ok_status();
}

static iree_status_t loom_aie2p_float_compare_build_packet(
    loom_builder_t* builder, loom_location_id_t location,
    loom_numeric_float_encoding_t encoding,
    const loom_aie2p_float_compare_plan_t* plan, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_type_t float_type, loom_value_id_t* out_result) {
  loom_aie2p_float_ordering_context_t ordering = {0};
  loom_aie2p_float_ordering_initialize(builder, float_type, encoding, location,
                                       &ordering);
  loom_value_id_t magnitude_mask = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_constant(
      &ordering, (int64_t)encoding.magnitude_mask, &magnitude_mask));
  loom_aie2p_float_ordering_value_t lhs_value = {0};
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_value(
      &ordering, lhs, magnitude_mask, &lhs_value));
  loom_aie2p_float_ordering_value_t rhs_value = {0};
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_value(
      &ordering, rhs, magnitude_mask, &rhs_value));
  loom_value_id_t special_magnitude = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_constant(
      &ordering, (int64_t)encoding.special_magnitude, &special_magnitude));
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_nan(
      &ordering, special_magnitude, &lhs_value));
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_nan(
      &ordering, special_magnitude, &rhs_value));
  loom_op_t* unordered_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_ori_build(
      ordering.builder, lhs_value.is_nan, rhs_value.is_nan,
      ordering.predicate_type, ordering.location, &unordered_op));
  const loom_value_id_t unordered = loom_vector_ori_result(unordered_op);

  loom_value_id_t result = unordered;
  if (plan->relation != LOOM_AIE2P_FLOAT_COMPARE_RELATION_UNORDERED) {
    loom_value_id_t zero = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_aie2p_float_ordering_build_constant(&ordering, 0, &zero));
    if (plan->relation == LOOM_AIE2P_FLOAT_COMPARE_RELATION_ORDERED) {
      loom_value_id_t all_true = LOOM_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_compare(
          &ordering, LOOM_VECTOR_CMPI_PREDICATE_EQ, zero, zero, &all_true));
      loom_op_t* invert_op = NULL;
      IREE_RETURN_IF_ERROR(loom_vector_xori_build(
          ordering.builder, all_true, unordered, ordering.predicate_type,
          ordering.location, &invert_op));
      result = loom_vector_xori_result(invert_op);
    } else {
      const uint64_t sign_bit = encoding.magnitude_mask + 1;
      const int64_t signed_sign_bit =
          encoding.integer_type == LOOM_SCALAR_TYPE_I64 ? INT64_MIN
                                                        : -(int64_t)sign_bit;
      loom_value_id_t sign_mask = LOOM_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_constant(
          &ordering, signed_sign_bit, &sign_mask));
      loom_value_id_t all_ones = LOOM_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(
          loom_aie2p_float_ordering_build_constant(&ordering, -1, &all_ones));
      IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_key(
          &ordering, zero, sign_mask, all_ones, &lhs_value));
      IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_key(
          &ordering, zero, sign_mask, all_ones, &rhs_value));
      if (encoding.integer_type == LOOM_SCALAR_TYPE_I64) {
        IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_normalize_zero(
            &ordering, zero, sign_mask, &lhs_value));
        IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_normalize_zero(
            &ordering, zero, sign_mask, &rhs_value));
      }
      loom_value_id_t base = LOOM_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_build_compare(
          &ordering, kAie2pFloatCompareIntegerPredicates[plan->relation],
          lhs_value.key, rhs_value.key, &base));

      loom_op_t* combine_op = NULL;
      if (plan->nan_policy == LOOM_AIE2P_FLOAT_COMPARE_NAN_POLICY_SET) {
        IREE_RETURN_IF_ERROR(loom_vector_ori_build(
            ordering.builder, base, unordered, ordering.predicate_type,
            ordering.location, &combine_op));
        result = loom_vector_ori_result(combine_op);
      } else {
        // Clear unordered lanes without materializing an all-true predicate:
        // base ^ (base & unordered) is equivalent to base & ~unordered.
        IREE_RETURN_IF_ERROR(loom_vector_andi_build(
            ordering.builder, base, unordered, ordering.predicate_type,
            ordering.location, &combine_op));
        const loom_value_id_t unordered_base =
            loom_vector_andi_result(combine_op);
        IREE_RETURN_IF_ERROR(loom_vector_xori_build(
            ordering.builder, base, unordered_base, ordering.predicate_type,
            ordering.location, &combine_op));
        result = loom_vector_xori_result(combine_op);
      }
    }
  }

  *out_result = result;
  return iree_ok_status();
}

iree_status_t loom_aie2p_legalize_vector_cmpf(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_NO_COMMENT,
  };
  if (context->descriptor_set != loom_aie2p_core_descriptor_set() ||
      !loom_target_legalization_op_has_source_vector_carriers(context, op)) {
    return iree_ok_status();
  }

  const loom_value_id_t lhs = loom_vector_cmpf_lhs(op);
  const loom_type_t float_type = loom_module_value_type(context->module, lhs);
  loom_numeric_float_encoding_t encoding = {0};
  const bool has_encoding = loom_numeric_float_encoding(
      loom_type_element_type(float_type), &encoding);
  IREE_ASSERT(has_encoding && "matched float type must have an encoding");
  (void)has_encoding;

  uint64_t lane_count_u64 = 0;
  const bool has_static_lane_count =
      loom_type_static_element_count(float_type, &lane_count_u64);
  IREE_ASSERT(has_static_lane_count && lane_count_u64 > 0 &&
              lane_count_u64 <= UINT32_MAX &&
              "source vector carrier must have a static lane count");
  (void)has_static_lane_count;
  const uint32_t lane_count = (uint32_t)lane_count_u64;
  const uint32_t element_bit_count =
      loom_scalar_type_bitwidth(loom_type_element_type(float_type));
  const uint32_t packet_lane_count =
      LOOM_AIE2P_FLOAT_ORDERING_PACKET_BIT_COUNT / element_bit_count;
  const uint32_t packet_count =
      (lane_count + packet_lane_count - 1u) / packet_lane_count;

  loom_rewriter_t* rewriter = context->rewriter;
  loom_builder_set_before(&rewriter->builder, op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(rewriter);
  const loom_aie2p_float_compare_plan_t plan =
      kAie2pFloatComparePlans[loom_vector_cmpf_predicate(op)];
  loom_value_id_t linear_lhs = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_linearize_value(
      &rewriter->builder, lhs, float_type, lane_count, op->location,
      &linear_lhs));
  loom_value_id_t linear_rhs = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_linearize_value(
      &rewriter->builder, loom_vector_cmpf_rhs(op), float_type, lane_count,
      op->location, &linear_rhs));

  loom_value_id_t* packets = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      context->arena, packet_count, sizeof(*packets), (void**)&packets));
  for (uint32_t packet_index = 0; packet_index < packet_count; ++packet_index) {
    const uint32_t lane_offset = packet_index * packet_lane_count;
    const uint32_t current_lane_count =
        iree_min(lane_count - lane_offset, packet_lane_count);
    loom_value_id_t packet_lhs = linear_lhs;
    loom_value_id_t packet_rhs = linear_rhs;
    if (packet_count > 1) {
      IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_slice_value(
          &rewriter->builder, linear_lhs, float_type, lane_offset,
          current_lane_count, op->location, &packet_lhs));
      IREE_RETURN_IF_ERROR(loom_aie2p_float_ordering_slice_value(
          &rewriter->builder, linear_rhs, float_type, lane_offset,
          current_lane_count, op->location, &packet_rhs));
    }
    const loom_type_t packet_type =
        loom_aie2p_linear_vector_type(float_type, current_lane_count);
    IREE_RETURN_IF_ERROR(loom_aie2p_float_compare_build_packet(
        &rewriter->builder, op->location, encoding, &plan, packet_lhs,
        packet_rhs, packet_type, &packets[packet_index]));
  }

  const loom_type_t result_type =
      loom_module_value_type(context->module, loom_vector_cmpf_result(op));
  const loom_type_t linear_result_type =
      loom_aie2p_linear_vector_type(result_type, lane_count);
  loom_value_id_t replacement = packets[0];
  if (packet_count > 1) {
    loom_op_t* concat_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_concat_build(
        &rewriter->builder, /*axis=*/0, packets, packet_count,
        linear_result_type, op->location, &concat_op));
    replacement = loom_vector_concat_result(concat_op);
  }
  if (!loom_type_equal(linear_result_type, result_type)) {
    loom_op_t* bitcast_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_bitcast_build(
        &rewriter->builder, replacement, linear_result_type, result_type,
        op->location, &bitcast_op));
    replacement = loom_vector_bitcast_result(bitcast_op);
  }

  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement, 1));
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN,
  };
  return iree_ok_status();
}

#undef LOOM_AIE2P_FLOAT_ORDERING_PACKET_BIT_COUNT
