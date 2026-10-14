// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/amdgpu/kernel_emission.h"

#include <stdint.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/emit/native/amdgpu/encoding.h"
#include "loom/target/emit/native/amdgpu/kernel_assembly.h"
#include "loom/target/emit/native/amdgpu/kernel_entry.h"
#include "loom/target/reporting/low_mix.h"
#include "loom/target/reporting/low_names.h"

static iree_status_t loom_amdgpu_kernel_emission_record_native_insertions(
    loom_target_compile_report_t* report,
    const loom_low_emission_frame_t* frame,
    const loom_amdgpu_encoded_instruction_stream_t* stream) {
  if (report == NULL || stream->native_insertion_count == 0) {
    return iree_ok_status();
  }

  const loom_low_descriptor_set_t* descriptor_set =
      frame->schedule.target.descriptor_set;
  loom_target_compile_report_low_dynamic_context_t dynamic_context = {0};
  iree_status_t status =
      loom_target_compile_report_low_dynamic_context_initialize(
          frame, &dynamic_context);
  for (iree_host_size_t i = 0;
       i < stream->native_insertion_count && iree_status_is_ok(status); ++i) {
    const loom_amdgpu_native_insertion_t* insertion =
        &stream->native_insertions[i];
    const loom_low_schedule_node_t* node =
        &frame->schedule.nodes[insertion->node_index];
    const loom_low_schedule_block_t* block =
        &frame->schedule.blocks[insertion->block_index];
    loom_target_compile_report_target_insertion_kind_t insertion_kind =
        LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_NONE;
    iree_string_view_t packet_key = iree_string_view_empty();
    switch (insertion->kind) {
      case LOOM_AMDGPU_NATIVE_INSERTION_ADDRESS_STATE:
        insertion_kind = LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_STATE;
        break;
      case LOOM_AMDGPU_NATIVE_INSERTION_WAIT:
        insertion_kind = LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_WAIT;
        break;
      case LOOM_AMDGPU_NATIVE_INSERTION_S_NOP:
        insertion_kind = LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_DELAY;
        packet_key = IREE_SV("amdgpu.s_nop");
        break;
      case LOOM_AMDGPU_NATIVE_INSERTION_S_DELAY_ALU:
        insertion_kind = LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_DELAY;
        break;
      case LOOM_AMDGPU_NATIVE_INSERTION_V_NOP:
        insertion_kind = LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_DELAY;
        packet_key = IREE_SV("amdgpu.v_nop");
        break;
      case LOOM_AMDGPU_NATIVE_INSERTION_BRANCH_ISLAND_SKIP:
      case LOOM_AMDGPU_NATIVE_INSERTION_BRANCH_ISLAND_HOP:
        insertion_kind = LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_OTHER;
        packet_key = IREE_SV("amdgpu.s_branch");
        break;
      case LOOM_AMDGPU_NATIVE_INSERTION_NONE:
      default:
        IREE_ASSERT_UNREACHABLE(
            "native encoder produced an invalid insertion kind");
        IREE_BUILTIN_UNREACHABLE();
    }
    if (insertion->descriptor_ref != LOOM_AMDGPU_DESCRIPTOR_REF_NONE) {
      const loom_low_descriptor_t* packet_descriptor =
          loom_amdgpu_descriptor_ref_descriptor(descriptor_set,
                                                insertion->descriptor_ref);
      IREE_ASSERT(packet_descriptor != NULL);
      packet_key = loom_low_descriptor_set_string(
          descriptor_set, packet_descriptor->key_string_ref);
    }
    iree_string_view_t boundary_descriptor_key = iree_string_view_empty();
    if (node->descriptor != NULL) {
      boundary_descriptor_key = loom_low_descriptor_set_string(
          descriptor_set, node->descriptor->key_string_ref);
    }
    loom_target_compile_report_target_insertion_row_t row = {
        .function_name = report->function_name,
        .insertion_kind = insertion_kind,
        .packet_key = packet_key,
        .block_name =
            loom_target_compile_report_block_name(frame->module, block->block),
        .block_index = insertion->block_index,
        .node_index = insertion->node_index,
        .scheduled_ordinal = insertion->scheduled_ordinal,
        .boundary_operation_name = node->op != NULL
                                       ? loom_op_name(frame->module, node->op)
                                       : iree_string_view_empty(),
        .boundary_descriptor_key = boundary_descriptor_key,
        .static_packet_count = 1,
    };
    uint64_t execution_multiplier = 0;
    if (insertion->kind != LOOM_AMDGPU_NATIVE_INSERTION_BRANCH_ISLAND_HOP &&
        loom_target_compile_report_low_node_execution_multiplier(
            frame->module, &dynamic_context, node, &execution_multiplier)) {
      row.flags =
          LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_FLAG_DYNAMIC_PACKET_COUNT;
      row.dynamic_packet_count = execution_multiplier;
    }
    status =
        loom_target_compile_report_record_target_insertion_row(report, &row);
  }
  loom_target_compile_report_low_dynamic_context_deinitialize(&dynamic_context);
  return status;
}

static iree_status_t loom_amdgpu_kernel_emission_record_summary(
    loom_target_compile_report_t* report,
    const loom_amdgpu_kernel_record_t* record,
    const loom_amdgpu_encoded_instruction_stream_t* stream,
    const loom_amdgpu_kernel_entry_envelope_t* entry_envelope,
    const loom_amdgpu_packet_plan_t* packet_plan,
    iree_const_byte_span_t kernel_text) {
  if (report == NULL) {
    return iree_ok_status();
  }

  uint64_t instruction_count = 0;
  if (!iree_checked_add_u64(stream->instruction_count,
                            entry_envelope->instruction_count,
                            &instruction_count)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "AMDGPU kernel entry instruction count overflowed");
  }
  const uint64_t coissued_instruction_count =
      (uint64_t)packet_plan->vopd_plan.pair_count;
  loom_target_compile_report_record_emission(report, instruction_count,
                                             kernel_text.data_length,
                                             kernel_text.data_length);
  const loom_target_compile_report_emission_breakdown_t emission_breakdown = {
      .body_instruction_count = stream->instruction_count,
      .entry_instruction_count = entry_envelope->instruction_count,
      .coissued_instruction_count = coissued_instruction_count,
      .coissued_component_count = coissued_instruction_count * 2u,
  };
  loom_target_compile_report_record_emission_breakdown(report,
                                                       &emission_breakdown);
  loom_target_compile_report_record_memory(
      report, record->metadata.private_segment_fixed_size,
      record->metadata.group_segment_fixed_size);
  return iree_ok_status();
}

iree_status_t loom_amdgpu_kernel_emission_build(
    const loom_low_emission_frame_t* frame,
    const loom_amdgpu_kernel_plan_t* plan,
    iree_string_builder_t* target_listing, loom_target_compile_report_t* report,
    loom_amdgpu_hsaco_kernel_t* out_kernel,
    iree_arena_allocator_t* table_arena) {
  *out_kernel = (loom_amdgpu_hsaco_kernel_t){0};
  const loom_amdgpu_kernel_record_t* record = &plan->record;
  const loom_amdgpu_packet_plan_t* packet_plan = &plan->packet_plan;

  loom_amdgpu_encoded_instruction_stream_t stream = {0};
  const loom_amdgpu_encode_instruction_stream_options_t encode_options = {
      .packet_plan = packet_plan,
      .storage_layout = &record->storage_layout,
      .flags =
          report != NULL
              ? LOOM_AMDGPU_ENCODE_INSTRUCTION_STREAM_FLAG_CAPTURE_NATIVE_INSERTIONS
              : LOOM_AMDGPU_ENCODE_INSTRUCTION_STREAM_FLAG_NONE,
  };
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_encode_instruction_stream_result_with_options(
          &frame->schedule, &frame->allocation, &encode_options, &stream,
          table_arena));

  const loom_amdgpu_kernel_entry_envelope_t* entry_envelope =
      loom_amdgpu_kernel_entry_envelope_for_properties(
          &record->processor->properties);
  iree_const_byte_span_t kernel_text = iree_const_byte_span_empty();
  const loom_amdgpu_hsaco_text_fixup_t* kernel_text_fixups = NULL;
  IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_entry_prepend_text(
      entry_envelope, stream.text, stream.text_fixups, stream.text_fixup_count,
      &kernel_text, &kernel_text_fixups, table_arena));
  const loom_amdgpu_hsaco_kernel_t kernel = {
      .metadata = record->metadata,
      .descriptor_options =
          {
              .flags = record->descriptor_flags,
              .minimum_user_sgpr_count = record->user_sgpr_count,
          },
      .text = kernel_text,
      .text_fixups = kernel_text_fixups,
      .text_fixup_count = stream.text_fixup_count,
  };

  if (target_listing != NULL) {
    if (iree_string_builder_size(target_listing) != 0) {
      IREE_RETURN_IF_ERROR(
          iree_string_builder_append_cstring(target_listing, "\n\n"));
    }
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_kernel_assembly(
        &frame->schedule, &frame->allocation, record, packet_plan,
        &stream.layout, target_listing, table_arena));
  }

  IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_emission_record_native_insertions(
      report, frame, &stream));
  if (report != NULL) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_emission_record_summary(
        report, record, &stream, entry_envelope, packet_plan, kernel_text));
  }
  *out_kernel = kernel;
  return iree_ok_status();
}
