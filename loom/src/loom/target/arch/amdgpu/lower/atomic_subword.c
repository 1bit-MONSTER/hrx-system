// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/atomic_subword.h"

#include "loom/ir/module.h"
#include "loom/ops/view/ops.h"
#include "loom/target/arch/amdgpu/lower/bitpack.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/topology.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"

iree_status_t loom_amdgpu_emit_subword_cmpxchg(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_atomic_plan_t* plan, loom_value_id_t address,
    loom_value_id_t expected, loom_value_id_t replacement,
    loom_named_attr_slice_t packet_attrs, loom_value_id_t* out_old) {
  loom_builder_t* builder = loom_low_lower_context_builder(context);
  const loom_location_id_t location = source_op->location;
  const uint32_t bit_count = plan->source.element_byte_count * 8;
  const uint32_t payload_mask = (1u << bit_count) - 1u;
  loom_type_t vgpr_type = loom_type_none();
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_vgpr_type(context, &vgpr_type));
  loom_type_t mask_type = loom_type_none();
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_make_sgpr_range_type(context, 2, &mask_type));

  // Flat address assembly already includes every logical offset. LDS address
  // assembly leaves the descriptor's static offset for this emitter.
  const bool flat_address =
      plan->address_form == LOOM_AMDGPU_MEMORY_ADDRESS_FORM_FLAT;
  loom_type_t vgpr_x2_type = loom_type_none();
  loom_value_id_t address_high = LOOM_VALUE_ID_INVALID;
  if (flat_address) {
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_make_vgpr_range_type(context, 2, &vgpr_x2_type));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_slice(
        context, source_op, address, 1, vgpr_type, &address_high));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_slice(context, source_op, address,
                                                    0, vgpr_type, &address));
  } else if (plan->source.static_byte_offset) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32_LIT, address,
        (uint32_t)plan->source.static_byte_offset, vgpr_type, &address));
  }
  loom_value_id_t word_address = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT, address,
      ~3u, vgpr_type, &word_address));
  if (flat_address) {
    const loom_value_id_t address_parts[] = {word_address, address_high};
    loom_op_t* concat_op = NULL;
    IREE_RETURN_IF_ERROR(loom_low_concat_build(
        builder, address_parts, IREE_ARRAYSIZE(address_parts), vgpr_x2_type,
        location, &concat_op));
    word_address = loom_low_concat_result(concat_op);
  }
  loom_value_id_t shift = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT, address, 3u,
      vgpr_type, &shift));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32_LIT, shift,
      3u, vgpr_type, &shift));
  loom_value_id_t mask = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32, payload_mask,
      vgpr_type, &mask));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32, shift, mask,
      vgpr_type, &mask));
  loom_value_id_t neighbor_mask = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_XOR_B32_LIT, mask,
      UINT32_MAX, vgpr_type, &neighbor_mask));

  // Narrow carriers may have sign bits or undefined register-part bits above
  // their payload. Only the logical expected/replacement bits enter the word.
  IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_full_low_vgpr_b32(
      context, source_op, expected, &expected));
  IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_full_low_vgpr_b32(
      context, source_op, replacement, &replacement));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT, expected,
      payload_mask, vgpr_type, &expected));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT, replacement,
      payload_mask, vgpr_type, &replacement));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32, shift,
      expected, vgpr_type, &expected));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32, shift,
      replacement, vgpr_type, &replacement));

  loom_value_id_t initial_word = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32, 0, vgpr_type,
      &initial_word));
  loom_op_t* packet = NULL;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_op(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B64_EXEC_READ,
      /*operands=*/NULL, /*operand_count=*/0, loom_named_attr_slice_empty(),
      &mask_type, 1, &packet));
  const loom_value_id_t entry_exec = loom_low_op_results(packet).values[0];

  loom_block_t* entry_block = builder->ip.block;
  loom_block_t* loop_block = NULL;
  IREE_RETURN_IF_ERROR(loom_region_insert_block(
      builder->module, entry_block->parent_region,
      (uint16_t)(entry_block->region_index + 1), &loop_block));
  loom_block_t* retry_block = NULL;
  IREE_RETURN_IF_ERROR(loom_region_insert_block(
      builder->module, loop_block->parent_region,
      (uint16_t)(loop_block->region_index + 1), &retry_block));
  loom_block_t* exit_block = NULL;
  IREE_RETURN_IF_ERROR(loom_region_insert_block(
      builder->module, retry_block->parent_region,
      (uint16_t)(retry_block->region_index + 1), &exit_block));
  loom_value_id_t previous_word = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_builder_define_block_arg(
      builder, loop_block, vgpr_type, &previous_word));
  loom_value_id_t pending = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_builder_define_block_arg(builder, loop_block, mask_type, &pending));
  const loom_value_id_t initial_args[] = {initial_word, entry_exec};
  IREE_RETURN_IF_ERROR(loom_low_br_build(builder, loop_block, initial_args,
                                         IREE_ARRAYSIZE(initial_args), location,
                                         &packet));

  loom_builder_set_block(builder, loop_block);
  loom_value_id_t neighbors = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32, previous_word,
      neighbor_mask, vgpr_type, &neighbors));
  loom_value_id_t expected_word = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32, neighbors,
      expected, vgpr_type, &expected_word));
  loom_value_id_t replacement_word = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32, neighbors,
      replacement, vgpr_type, &replacement_word));

  // Retired lanes must not execute another CAS: even a redundant successful
  // modification could change the logical operation's synchronization effects.
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_op(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B64_EXEC, &pending,
      1, loom_named_attr_slice_empty(), /*result_types=*/NULL,
      /*result_count=*/0, &packet));
  loom_value_id_t operands[] = {word_address, expected_word, replacement_word};
  iree_host_size_t operand_count = IREE_ARRAYSIZE(operands);
  if (flat_address) {
    // Flat compare-exchange consumes replacement followed by expected bits.
    const loom_value_id_t pair_parts[] = {replacement_word, expected_word};
    loom_op_t* concat_op = NULL;
    IREE_RETURN_IF_ERROR(
        loom_low_concat_build(builder, pair_parts, IREE_ARRAYSIZE(pair_parts),
                              vgpr_x2_type, location, &concat_op));
    operands[1] = loom_low_concat_result(concat_op);
    operand_count = 2;
  }
  IREE_RETURN_IF_ERROR(loom_low_lower_emit_resolved_descriptor_op(
      context, &plan->descriptor, operands, operand_count, packet_attrs,
      &vgpr_type, 1, /*tied_results=*/NULL,
      /*tied_result_count=*/0, location, &packet));
  const loom_value_id_t attempted_word = loom_low_op_results(packet).values[0];
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_op(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B64_EXEC,
      &entry_exec, 1, loom_named_attr_slice_empty(), /*result_types=*/NULL,
      /*result_count=*/0, &packet));
  loom_value_id_t observed_word = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_select(
      context, source_op, previous_word, attempted_word, pending, vgpr_type,
      &observed_word));

  // A changed neighbor cannot cause a spurious strong-CAS failure. A changed
  // logical payload can: that value is the failing operation's observation.
  loom_value_id_t word_changed = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_NE_I32,
      observed_word, expected_word, mask_type, &word_changed));
  loom_value_id_t observed_payload = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32, observed_word,
      mask, vgpr_type, &observed_payload));
  loom_value_id_t payload_matches = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32,
      observed_payload, expected, mask_type, &payload_matches));
  loom_value_id_t next_pending = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_AND_B64, word_changed,
      payload_matches, mask_type, &next_pending));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_AND_B64, next_pending,
      pending, mask_type, &next_pending));
  loom_value_id_t retry = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_lane_mask_nonzero_scc(
      context, source_op, next_pending,
      loom_amdgpu_target_wavefront_size(loom_low_lower_context_bundle(context)),
      &retry));
  IREE_RETURN_IF_ERROR(loom_low_cond_br_build(builder, retry, retry_block,
                                              exit_block, location, &packet));
  loom_builder_set_block(builder, retry_block);
  const loom_value_id_t retry_args[] = {observed_word, next_pending};
  IREE_RETURN_IF_ERROR(loom_low_br_build(builder, loop_block, retry_args,
                                         IREE_ARRAYSIZE(retry_args), location,
                                         &packet));

  loom_builder_set_block(builder, exit_block);
  loom_value_id_t shifted_old = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32, shift,
      observed_word, vgpr_type, &shifted_old));
  const loom_type_t source_type = loom_module_value_type(
      builder->module, loom_view_atomic_cmpxchg_old(source_op));
  const loom_amdgpu_bitfield_extract_mode_t extract_mode =
      loom_scalar_type_set_contains(LOOM_SCALAR_TYPE_SET_INTEGER,
                                    loom_type_element_type(source_type))
          ? LOOM_AMDGPU_BITFIELD_EXTRACT_MODE_SIGN_EXTEND
          : LOOM_AMDGPU_BITFIELD_EXTRACT_MODE_ZERO_EXTEND;
  return loom_amdgpu_extract_vgpr_bitfield(context, source_op, shifted_old,
                                           /*bit_offset=*/0, bit_count,
                                           extract_mode, vgpr_type, out_old);
}
