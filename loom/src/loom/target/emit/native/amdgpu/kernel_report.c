// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/amdgpu/kernel_report.h"

#include <stdint.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/reporting/low_mix.h"
#include "loom/target/reporting/low_names.h"

static iree_status_t loom_amdgpu_kernel_report_record_native_insertions(
    loom_target_compile_report_t* report,
    const loom_low_emission_frame_t* frame,
    const loom_target_compile_report_low_dynamic_context_t* dynamic_context,
    const loom_amdgpu_kernel_emission_result_t* result) {
  if (result->native_insertion_count == 0) {
    return iree_ok_status();
  }

  const loom_low_descriptor_set_t* descriptor_set =
      frame->schedule.target.descriptor_set;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < result->native_insertion_count; ++i) {
    const loom_amdgpu_native_insertion_t* insertion =
        &result->native_insertions[i];
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
            frame->module, dynamic_context, node, &execution_multiplier)) {
      row.flags =
          LOOM_TARGET_COMPILE_REPORT_TARGET_INSERTION_FLAG_DYNAMIC_PACKET_COUNT;
      row.dynamic_packet_count = execution_multiplier;
    }
    status =
        loom_target_compile_report_record_target_insertion_row(report, &row);
    if (!iree_status_is_ok(status)) {
      break;
    }
  }
  return status;
}

static iree_status_t loom_amdgpu_kernel_report_record_summary(
    loom_target_compile_report_t* report, const loom_amdgpu_kernel_plan_t* plan,
    const loom_amdgpu_kernel_emission_result_t* result) {
  uint64_t instruction_count = 0;
  if (!iree_checked_add_u64(result->body_instruction_count,
                            result->entry_instruction_count,
                            &instruction_count)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "AMDGPU kernel instruction count overflowed");
  }
  const uint64_t coissued_instruction_count =
      (uint64_t)plan->packet_plan.vopd_plan.pair_count;
  const uint64_t text_length = result->kernel.text.data_length;
  loom_target_compile_report_record_emission(report, instruction_count,
                                             text_length, text_length);
  const loom_target_compile_report_emission_breakdown_t emission_breakdown = {
      .body_instruction_count = result->body_instruction_count,
      .entry_instruction_count = result->entry_instruction_count,
      .coissued_instruction_count = coissued_instruction_count,
      .coissued_component_count = coissued_instruction_count * 2u,
  };
  loom_target_compile_report_record_emission_breakdown(report,
                                                       &emission_breakdown);
  loom_target_compile_report_record_memory(
      report, plan->record.metadata.private_segment_fixed_size,
      plan->record.metadata.group_segment_fixed_size);
  return iree_ok_status();
}

iree_status_t loom_amdgpu_kernel_report_record_emission(
    loom_target_compile_report_t* report,
    const loom_low_emission_frame_t* frame,
    const loom_target_compile_report_low_dynamic_context_t* dynamic_context,
    const loom_amdgpu_kernel_plan_t* plan,
    const loom_amdgpu_kernel_emission_result_t* result) {
  IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_report_record_native_insertions(
      report, frame, dynamic_context, result));
  return loom_amdgpu_kernel_report_record_summary(report, plan, result);
}
