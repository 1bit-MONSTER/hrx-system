// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/fragment_memory/access.h"

#include <stdint.h>

#include "loom/codegen/low/descriptors.h"
#include "loom/codegen/low/lower/source_memory.h"
#include "loom/ir/attribute.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/amdgpu/lower/buffer_descriptor.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/memory.h"
#include "loom/target/arch/amdgpu/lower/types.h"

iree_status_t loom_amdgpu_fragment_memory_packet_type(
    loom_low_lower_context_t* context, uint16_t packet_register_count,
    loom_type_t vgpr_type, loom_type_t* out_type) {
  if (packet_register_count == 1) {
    *out_type = vgpr_type;
    return iree_ok_status();
  }
  return loom_amdgpu_make_vgpr_range_type(context, packet_register_count,
                                          out_type);
}

static bool loom_amdgpu_fragment_memory_uses_buffer_descriptor(
    const loom_amdgpu_fragment_memory_plan_t* plan) {
  return plan->source.memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_DESCRIPTOR;
}

iree_status_t loom_amdgpu_fragment_memory_packet_resource(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan, loom_value_id_t low_binding,
    loom_value_id_t* out_low_packet_resource,
    loom_value_id_t* out_low_soffset) {
  *out_low_packet_resource = low_binding;
  *out_low_soffset = LOOM_VALUE_ID_INVALID;
  if (!loom_amdgpu_fragment_memory_uses_buffer_descriptor(plan)) {
    if (plan->scalar_base.dynamic_term_mask == 0 &&
        plan->scalar_base.byte_offset == 0) {
      return iree_ok_status();
    }
    loom_amdgpu_memory_dynamic_term_sequence_t sequence = {0};
    for (uint8_t i = 0; i < plan->source.dynamic_term_count; ++i) {
      if (iree_any_bit_set(plan->scalar_base.dynamic_term_mask, UINT32_C(1)
                                                                    << i)) {
        sequence.terms[sequence.count] = &plan->source.dynamic_terms[i];
        sequence.plans[sequence.count] = &plan->dynamic_term_plans[i];
        sequence.kinds[sequence.count++] =
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
      }
    }
    return loom_amdgpu_emit_sgpr_base_byte_offset_terms(
        context, source_op, &sequence, plan->scalar_base.byte_offset,
        low_binding, out_low_packet_resource);
  }

  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_hal_buffer_descriptor(
      context, source_op, low_binding, &plan->source, plan->buffer_extent,
      out_low_packet_resource));
  loom_type_t sgpr_type = loom_type_none();
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_sgpr_type(context, &sgpr_type));
  return loom_amdgpu_emit_const_u32(context, source_op,
                                    LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32, 0,
                                    sgpr_type, out_low_soffset);
}

static iree_status_t loom_amdgpu_record_fragment_memory_packet(
    loom_low_lower_context_t* context, const loom_op_t* low_op,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    const loom_amdgpu_fragment_memory_packet_plan_t* packet,
    loom_amdgpu_descriptor_ref_t descriptor_ref, uint16_t element_index,
    uint32_t vector_lane_count) {
  int64_t static_byte_offset;
  if (!loom_amdgpu_fragment_memory_static_offset_i64(
          plan, packet->register_index, element_index, &static_byte_offset)) {
    return iree_ok_status();
  }
  const loom_low_descriptor_t* descriptor =
      loom_amdgpu_descriptor_ref_descriptor(
          loom_low_lower_context_descriptor_set(context), descriptor_ref);
  const loom_low_lower_memory_packet_geometry_t geometry = {
      .static_byte_offset = static_byte_offset,
      .element_byte_count = plan->element_byte_count,
      .vector_lane_count = vector_lane_count,
      .vector_lane_byte_stride = plan->element_byte_count,
  };
  const loom_amdgpu_fragment_memory_packet_offset_t runtime_packet_offset =
      loom_amdgpu_fragment_memory_runtime_packet_offset(
          plan, packet->register_index, element_index);
  return loom_low_lower_record_memory_packet(
      context, low_op, descriptor, loom_low_lower_source_memory_origin(context),
      &plan->source, &geometry, runtime_packet_offset.byte_facts);
}

static iree_status_t loom_amdgpu_make_fragment_memory_attrs(
    loom_low_lower_context_t* context, loom_named_attr_t* attrs,
    iree_host_size_t attr_capacity, int64_t immediate_offset,
    iree_host_size_t* out_attr_count) {
  *out_attr_count = 0;
  return loom_amdgpu_append_i64_attr(context, IREE_SV("offset"),
                                     immediate_offset, attrs, attr_capacity,
                                     out_attr_count);
}

static iree_status_t loom_amdgpu_emit_fragment_load_packet_with_descriptor(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    const loom_amdgpu_fragment_memory_packet_plan_t* packet,
    loom_amdgpu_descriptor_ref_t descriptor_ref,
    loom_value_id_t low_tied_source, uint16_t element_index,
    uint32_t vector_lane_count, loom_type_t result_type,
    const loom_amdgpu_fragment_memory_address_t* address,
    loom_value_id_t low_resource, loom_value_id_t low_soffset,
    loom_value_id_t* out_low_packet) {
  *out_low_packet = LOOM_VALUE_ID_INVALID;
  loom_named_attr_t attrs[1] = {0};
  iree_host_size_t attr_count = 0;
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_fragment_memory_attrs(
      context, attrs, IREE_ARRAYSIZE(attrs), address->immediate_offset,
      &attr_count));

  loom_low_lower_resolved_descriptor_t descriptor = {0};
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_resolve_descriptor_ref(context, descriptor_ref, &descriptor));

  loom_value_id_t operands[4] = {0};
  iree_host_size_t operand_count = 0;
  if (low_tied_source != LOOM_VALUE_ID_INVALID) {
    operands[operand_count++] = low_tied_source;
  }
  if (loom_amdgpu_fragment_memory_uses_buffer_descriptor(plan)) {
    operands[operand_count++] = low_resource;
    operands[operand_count++] = address->low_vaddr;
    operands[operand_count++] = low_soffset;
  } else {
    operands[operand_count++] = address->low_vaddr;
    if (plan->source.memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP) {
      operands[operand_count++] = low_resource;
    }
  }
  loom_op_t* low_op = NULL;
  const loom_tied_result_t tied_result = {
      .result_index = 0,
      .operand_index = 0,
      .has_type_change = false,
  };
  const loom_tied_result_t* tied_results =
      low_tied_source != LOOM_VALUE_ID_INVALID ? &tied_result : NULL;
  const iree_host_size_t tied_result_count =
      low_tied_source != LOOM_VALUE_ID_INVALID ? 1 : 0;
  IREE_RETURN_IF_ERROR(loom_low_lower_emit_resolved_descriptor_op(
      context, &descriptor, operands, operand_count,
      loom_make_named_attr_slice(attrs, attr_count), &result_type, 1,
      tied_results, tied_result_count, source_op->location, &low_op));
  IREE_RETURN_IF_ERROR(loom_amdgpu_record_fragment_memory_packet(
      context, low_op, plan, packet, descriptor_ref, element_index,
      vector_lane_count));
  *out_low_packet = loom_value_slice_get(loom_low_op_results(low_op), 0);
  return iree_ok_status();
}

iree_status_t loom_amdgpu_emit_fragment_load_packet(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    const loom_amdgpu_fragment_memory_packet_plan_t* packet,
    uint16_t element_index, uint32_t vector_lane_count, loom_type_t result_type,
    const loom_amdgpu_fragment_memory_address_t* address,
    loom_value_id_t low_resource, loom_value_id_t low_soffset,
    loom_value_id_t* out_low_packet) {
  return loom_amdgpu_emit_fragment_load_packet_with_descriptor(
      context, source_op, plan, packet, packet->descriptor_ref,
      LOOM_VALUE_ID_INVALID, element_index, vector_lane_count, result_type,
      address, low_resource, low_soffset, out_low_packet);
}

iree_status_t loom_amdgpu_emit_fragment_load_high_half_packet(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    const loom_amdgpu_fragment_memory_packet_plan_t* packet,
    uint16_t element_index, uint32_t vector_lane_count, loom_type_t result_type,
    const loom_amdgpu_fragment_memory_address_t* address,
    loom_value_id_t low_partial_packet, loom_value_id_t low_resource,
    loom_value_id_t low_soffset, loom_value_id_t* out_low_packet) {
  return loom_amdgpu_emit_fragment_load_packet_with_descriptor(
      context, source_op, plan, packet, plan->packed_b16_high_descriptor_ref,
      low_partial_packet, element_index, vector_lane_count, result_type,
      address, low_resource, low_soffset, out_low_packet);
}

iree_status_t loom_amdgpu_emit_fragment_memory_low_subword_load_packet(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_value_id_t low_packet, loom_type_t vgpr_type,
    loom_value_id_t* out_full_packet) {
  *out_full_packet = LOOM_VALUE_ID_INVALID;
  if (loom_amdgpu_low_value_defines_vgpr_low16(context, low_packet)) {
    return loom_amdgpu_emit_vgpr_unary(
        context, source_op,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_BFE_U32_OFFSET_0_WIDTH_INLINE_LOW16,
        low_packet, vgpr_type, out_full_packet);
  }

  IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_low_vgpr_b32(
      context, source_op, low_packet, out_full_packet));
  return loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
      *out_full_packet, UINT32_C(0xFFFF), vgpr_type, out_full_packet);
}

iree_status_t loom_amdgpu_emit_fragment_store_packet(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    const loom_amdgpu_fragment_memory_packet_plan_t* packet,
    uint16_t element_index, uint32_t vector_lane_count,
    const loom_amdgpu_fragment_memory_address_t* address,
    loom_value_id_t low_payload_register, loom_value_id_t low_resource,
    loom_value_id_t low_soffset) {
  loom_named_attr_t attrs[1] = {0};
  iree_host_size_t attr_count = 0;
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_fragment_memory_attrs(
      context, attrs, IREE_ARRAYSIZE(attrs), address->immediate_offset,
      &attr_count));

  loom_low_lower_resolved_descriptor_t descriptor = {0};
  IREE_RETURN_IF_ERROR(loom_amdgpu_resolve_descriptor_ref(
      context, packet->descriptor_ref, &descriptor));

  loom_value_id_t operands[4] = {0};
  iree_host_size_t operand_count = 0;
  if (loom_amdgpu_fragment_memory_uses_buffer_descriptor(plan)) {
    operands[operand_count++] = low_payload_register;
    operands[operand_count++] = low_resource;
    operands[operand_count++] = address->low_vaddr;
    operands[operand_count++] = low_soffset;
  } else {
    operands[operand_count++] = address->low_vaddr;
    operands[operand_count++] = low_payload_register;
    if (plan->source.memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP) {
      operands[operand_count++] = low_resource;
    }
  }
  loom_op_t* low_op = NULL;
  IREE_RETURN_IF_ERROR(loom_low_lower_emit_resolved_descriptor_op(
      context, &descriptor, operands, operand_count,
      loom_make_named_attr_slice(attrs, attr_count), /*result_types=*/NULL,
      /*result_count=*/0, /*tied_results=*/NULL, /*tied_result_count=*/0,
      source_op->location, &low_op));
  return loom_amdgpu_record_fragment_memory_packet(
      context, low_op, plan, packet, packet->descriptor_ref, element_index,
      vector_lane_count);
}
