// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Source-value projection for generated source-to-Low lowering rules.
//
// Generated rule tables address source operands and results through compact
// value-ref rows. Matching and emission both consume those rows and must agree
// on how source values and scalar facts are projected. This component owns that
// shared interpretation; it does not select rules or emit target-Low IR.

#ifndef LOOM_CODEGEN_LOW_LOWER_RULE_VALUE_H_
#define LOOM_CODEGEN_LOW_LOWER_RULE_VALUE_H_

#include "iree/base/api.h"
#include "loom/codegen/low/lower/rules.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_unsigned_divisor_magic_info_t {
  // Multiplication constant consumed by the unsigned quotient recipe.
  uint64_t multiplier;
  // Final logical right shift applied after the high-half multiply.
  uint8_t post_shift;
  // Whether the quotient recipe needs the unsigned add-adjustment step.
  bool is_add;
} loom_low_lower_unsigned_divisor_magic_info_t;

// Exact signed constant-divisor recipe for a native-width high multiply.
typedef struct loom_low_lower_signed_divisor_magic_info_t {
  // Two's-complement multiplication constant at the selected integer width.
  uint64_t multiplier;
  // Final arithmetic right shift applied after the high-half multiply.
  uint8_t post_shift;
  // Numerator coefficient added to the high product: -1, 0, or 1.
  int8_t numerator_factor;
} loom_low_lower_signed_divisor_magic_info_t;

// Arithmetic shape of an exact signed reciprocal recipe. Adjacent pairs
// distinguish whether the high product needs an arithmetic post-shift.
typedef enum loom_low_lower_signed_divisor_magic_kind_e {
  LOOM_LOW_LOWER_SIGNED_DIVISOR_MAGIC_MULTIPLY = 0,
  LOOM_LOW_LOWER_SIGNED_DIVISOR_MAGIC_MULTIPLY_SHIFT = 1,
  LOOM_LOW_LOWER_SIGNED_DIVISOR_MAGIC_MULTIPLY_ADD = 2,
  LOOM_LOW_LOWER_SIGNED_DIVISOR_MAGIC_MULTIPLY_ADD_SHIFT = 3,
  LOOM_LOW_LOWER_SIGNED_DIVISOR_MAGIC_MULTIPLY_SUBTRACT = 4,
  LOOM_LOW_LOWER_SIGNED_DIVISOR_MAGIC_MULTIPLY_SUBTRACT_SHIFT = 5,
} loom_low_lower_signed_divisor_magic_kind_t;

// Derives an exact signed constant-divisor recipe using a |bit_width|-bit
// signed high-half multiply. The width is in [3, 64], and |divisor| must fit
// that signed width and must not be 0, 1, or -1. With q =
// signed_high_bit_width(n * multiplier), the quotient is:
//
//   q = (q + numerator_factor * n) >> post_shift;
//   q += logical_shift_right(q, bit_width - 1);
//
// All arithmetic before the shifts wraps at |bit_width| bits.
loom_low_lower_signed_divisor_magic_info_t
loom_low_lower_signed_divisor_magic_info(int64_t divisor, uint32_t bit_width);

// Returns the arithmetic shape represented by |info|.
loom_low_lower_signed_divisor_magic_kind_t
loom_low_lower_signed_divisor_magic_kind(
    loom_low_lower_signed_divisor_magic_info_t info);

// Derives an exact unsigned constant-divisor recipe using a |bit_width|-bit
// high-half multiply. The width is in [2, 64]. Divisor is in [2, 2^bit_width -
// 1] and numerator_maximum is an inclusive bound in [0, 2^bit_width - 1]. The
// bound restricts the input domain, not the width of the high-half
// multiplication. With q = high_bit_width(n * multiplier), the quotient is q >>
// post_shift, or (((n - q) >> 1) + q) >> post_shift when is_add is set. A zero
// multiplier represents an always-zero quotient. Every input bit participates;
// the range proof permits a smaller reciprocal without truncating the
// numerator.
loom_low_lower_unsigned_divisor_magic_info_t
loom_low_lower_unsigned_divisor_magic_info(uint64_t divisor, uint32_t bit_width,
                                           uint64_t numerator_maximum);

// Arithmetic shape of an exact unsigned reciprocal recipe. The multiplier-only
// form consumes the high product directly without an identity post-shift.
typedef enum loom_low_lower_unsigned_divisor_magic_kind_e {
  LOOM_LOW_LOWER_UNSIGNED_DIVISOR_MAGIC_MULTIPLY = 0,
  LOOM_LOW_LOWER_UNSIGNED_DIVISOR_MAGIC_MULTIPLY_SHIFT = 1,
  LOOM_LOW_LOWER_UNSIGNED_DIVISOR_MAGIC_MULTIPLY_ADD_SHIFT = 2,
} loom_low_lower_unsigned_divisor_magic_kind_t;

// Returns ceil(2^64 / divisor), for a divisor in [2, UINT32_MAX]. For a u32
// numerator n, high64(n * reciprocal) is n / divisor and
// high64(low64(n * reciprocal) * divisor) is n % divisor. The full reciprocal
// precision is necessary for the remainder identity.
uint64_t loom_low_lower_u32_divisor_reciprocal(uint32_t divisor);

// Returns the target-owned materializer selected by a nonzero value-ref
// materializer index. Generated table indices are trusted.
const loom_low_lower_value_materializer_t*
loom_low_lower_rule_value_materializer(
    const loom_low_lower_rule_set_t* rule_set,
    const loom_low_lower_value_ref_t* value_ref);

// Returns the source op containing the operand or result selected by a
// value-ref row. |source_nodes| contains the rule root at index zero when the
// rule spans multiple source operations; it may be NULL for root-only rules.
const loom_op_t* loom_low_lower_rule_source_op(
    const loom_low_lower_rule_set_t* rule_set, const loom_op_t* source_op,
    const loom_op_t* const* source_nodes, uint8_t source_node_count,
    uint16_t value_ref_index);

// Resolves an operand or result value-ref row across a selected source graph.
// Generated table indices and value-ref kinds are trusted.
loom_value_id_t loom_low_lower_rule_source_value_from_nodes(
    const loom_module_t* module, const loom_low_lower_rule_set_t* rule_set,
    const loom_op_t* source_op, const loom_op_t* const* source_nodes,
    uint8_t source_node_count, uint16_t value_ref_index);

// Resolves a source-backed value-ref across a selected source graph. Direct
// operand and result refs always resolve. Fact-derived refs return false when
// their producer-owned relation is unavailable or does not describe the whole
// referenced value. |vector_lane_projection| tests whole-value relations using
// the candidate representation without mutating source IR; an empty projection
// reads authored types.
bool loom_low_lower_rule_resolve_source_value_from_nodes(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    loom_target_contract_vector_lane_projection_t vector_lane_projection,
    const loom_low_lower_rule_set_t* rule_set, const loom_op_t* source_op,
    const loom_op_t* const* source_nodes, uint8_t source_node_count,
    uint16_t value_ref_index, loom_value_id_t* out_source_value_id);

// Resolves an operand or result value-ref row to its source SSA value.
// Generated table indices and value-ref kinds are trusted.
loom_value_id_t loom_low_lower_rule_source_value(
    const loom_module_t* module, const loom_low_lower_rule_set_t* rule_set,
    const loom_op_t* source_op, uint16_t value_ref_index);

// Resolves an operand or result value-ref row to the complete source field
// across a selected source graph. Non-source value refs return an empty span.
loom_value_slice_t loom_low_lower_rule_value_ref_field_span_from_nodes(
    const loom_module_t* module, const loom_low_lower_rule_set_t* rule_set,
    const loom_op_t* source_op, const loom_op_t* const* source_nodes,
    uint8_t source_node_count, uint16_t value_ref_index);

// Resolves an operand or result value-ref row to the complete source field.
// Non-source value refs return an empty span.
loom_value_slice_t loom_low_lower_rule_value_ref_field_span(
    const loom_module_t* module, const loom_low_lower_rule_set_t* rule_set,
    const loom_op_t* source_op, uint16_t value_ref_index);

// Projects scalar or uniform-vector integer facts for one source value.
bool loom_low_lower_rule_integer_immediate_facts(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    loom_value_id_t value_id, loom_value_facts_t* out_facts);

// Projects scalar or uniform-vector floating-point facts for one source value.
bool loom_low_lower_rule_float_immediate_facts(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    loom_value_id_t value_id, loom_value_facts_t* out_facts);

// Returns the inclusive unsigned maximum of a scalar numerator at its native
// product width. Unknown or sign-crossing facts retain the full unsigned
// domain.
uint64_t loom_low_lower_unsigned_numerator_maximum(
    const loom_value_fact_table_t* fact_table, loom_value_id_t numerator,
    uint32_t bit_width);

// Derives the unsigned 32-bit recipe from the numerator's retained range and an
// exact divisor. Returns false when the divisor is unavailable or outside
// [2, UINT32_MAX].
bool loom_low_lower_rule_value_facts_u32_divisor_magic_info(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    loom_value_id_t numerator, loom_value_id_t divisor,
    loom_low_lower_unsigned_divisor_magic_info_t* out_info);

// Derives the signed 32-bit recipe from an exact divisor. Returns false when
// the divisor is unavailable, outside the signed i32 domain, or equal to
// zero, one, or negative one.
bool loom_low_lower_rule_value_facts_i32_divisor_magic_info(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    loom_value_id_t divisor,
    loom_low_lower_signed_divisor_magic_info_t* out_info);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_RULE_VALUE_H_
