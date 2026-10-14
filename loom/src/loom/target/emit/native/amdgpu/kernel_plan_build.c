// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/amdgpu/kernel_plan_build.h"

#include <stdint.h>
#include <string.h>

#include "loom/codegen/low/diagnostics.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/target/arch/amdgpu/error_catalog.h"
#include "loom/target/arch/amdgpu/planning/occupancy.h"
#include "loom/target/arch/amdgpu/planning/wait_counters.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/emit/native/amdgpu/kernel_record_build.h"
#include "loom/target/emit/native/amdgpu/preflight.h"
#include "loom/target/reporting/low_names.h"

static void loom_amdgpu_kernel_plan_accumulate_wait_action(
    loom_target_compile_report_wait_plan_t* summary,
    const loom_amdgpu_wait_plan_action_t* action) {
  ++summary->action_count;
  switch (action->kind) {
    case LOOM_AMDGPU_WAIT_PLAN_ACTION_EXPLICIT:
      ++summary->explicit_action_count;
      break;
    case LOOM_AMDGPU_WAIT_PLAN_ACTION_PLANNED:
      ++summary->planned_action_count;
      break;
    case LOOM_AMDGPU_WAIT_PLAN_ACTION_UNKNOWN:
    default:
      IREE_ASSERT(false, "wait plan action kind must be known");
      break;
  }
  if (action->target_count == 0) {
    ++summary->full_drain_count;
    summary->max_full_drain_outstanding_before =
        iree_max(summary->max_full_drain_outstanding_before,
                 (uint64_t)action->outstanding_before);
  } else {
    ++summary->partial_wait_count;
  }
  const uint64_t drained_count =
      action->outstanding_before > action->target_count
          ? (uint64_t)action->outstanding_before - action->target_count
          : 0;
  summary->drained_count += drained_count;
  summary->max_drained_count =
      iree_max(summary->max_drained_count, drained_count);
  summary->max_outstanding_before = iree_max(
      summary->max_outstanding_before, (uint64_t)action->outstanding_before);
}

static iree_string_view_t loom_amdgpu_kernel_plan_wait_action_name(
    loom_amdgpu_wait_plan_action_kind_t kind) {
  switch (kind) {
    case LOOM_AMDGPU_WAIT_PLAN_ACTION_EXPLICIT:
      return IREE_SV("explicit");
    case LOOM_AMDGPU_WAIT_PLAN_ACTION_PLANNED:
      return IREE_SV("planned");
    case LOOM_AMDGPU_WAIT_PLAN_ACTION_UNKNOWN:
    default:
      return IREE_SV("unknown");
  }
}

typedef struct loom_amdgpu_kernel_plan_wait_endpoint_t {
  // Node index in the schedule table, or UINT32_MAX.
  uint32_t node_index;
  // Scheduled ordinal for |node_index|, or UINT32_MAX.
  uint32_t scheduled_ordinal;
  // Operation mnemonic for |node_index|, or empty.
  iree_string_view_t operation_name;
  // Descriptor key for |node_index|, or empty.
  iree_string_view_t descriptor_key;
  // Descriptor semantic tag for |node_index|, or empty.
  iree_string_view_t semantic_tag;
} loom_amdgpu_kernel_plan_wait_endpoint_t;

static loom_amdgpu_kernel_plan_wait_endpoint_t
loom_amdgpu_kernel_plan_wait_endpoint(const loom_amdgpu_wait_plan_t* wait_plan,
                                      uint32_t node_index) {
  loom_amdgpu_kernel_plan_wait_endpoint_t endpoint = {
      .node_index = UINT32_MAX,
      .scheduled_ordinal = UINT32_MAX,
      .operation_name = iree_string_view_empty(),
      .descriptor_key = iree_string_view_empty(),
      .semantic_tag = iree_string_view_empty(),
  };
  const loom_low_schedule_table_t* schedule = wait_plan->schedule;
  if (node_index == LOOM_LOW_SCHEDULE_NODE_NONE ||
      node_index >= schedule->node_count) {
    return endpoint;
  }

  const loom_low_schedule_node_t* node = &schedule->nodes[node_index];
  endpoint.node_index = node_index;
  endpoint.scheduled_ordinal = node->scheduled_ordinal;
  if (node->op != NULL) {
    endpoint.operation_name = loom_op_name(schedule->module, node->op);
  }
  if (node->descriptor != NULL) {
    endpoint.descriptor_key = loom_low_descriptor_set_string(
        schedule->target.descriptor_set, node->descriptor->key_string_ref);
    if (node->descriptor->semantic_tag_string_ref != LOOM_STRING_REF_NONE) {
      endpoint.semantic_tag = loom_low_descriptor_set_string(
          schedule->target.descriptor_set,
          node->descriptor->semantic_tag_string_ref);
    }
  }
  return endpoint;
}

static iree_status_t loom_amdgpu_kernel_plan_record_wait_plan(
    loom_target_compile_report_t* report,
    const loom_amdgpu_packet_plan_t* packet_plan) {
  if (report == NULL || packet_plan->wait_plan.action_count == 0) {
    return iree_ok_status();
  }
  const loom_amdgpu_wait_plan_t* wait_plan = &packet_plan->wait_plan;
  loom_target_compile_report_wait_plan_t summary = {0};
  loom_target_compile_report_wait_plan_t
      counter_summaries[LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT] = {0};
  loom_target_compile_report_wait_plan_t
      reason_summaries[LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT]
                      [LOOM_AMDGPU_WAIT_PLAN_REASON_COUNT] = {0};
  for (iree_host_size_t i = 0; i < wait_plan->action_count; ++i) {
    const loom_amdgpu_wait_plan_action_t* action = &wait_plan->actions[i];
    IREE_ASSERT(loom_amdgpu_wait_counter_id_is_valid(action->counter_id),
                "wait plan action must name a concrete counter");
    IREE_ASSERT(action->reason < LOOM_AMDGPU_WAIT_PLAN_REASON_COUNT,
                "wait plan action must name a concrete reason");
    loom_amdgpu_kernel_plan_accumulate_wait_action(&summary, action);
    const uint32_t counter_index =
        loom_amdgpu_wait_counter_slot_from_id(action->counter_id);
    loom_amdgpu_kernel_plan_accumulate_wait_action(
        &counter_summaries[counter_index], action);
    loom_amdgpu_kernel_plan_accumulate_wait_action(
        &reason_summaries[counter_index][action->reason], action);
  }
  loom_target_compile_report_record_wait_plan(report, &summary);
  for (uint32_t i = 0; i < LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT; ++i) {
    if (counter_summaries[i].action_count == 0) {
      continue;
    }
    const uint32_t counter_id = i + 1;
    const loom_target_compile_report_wait_counter_row_t row = {
        .function_name = report->function_name,
        .counter_name = loom_amdgpu_wait_counter_name(counter_id),
        .counter_id = counter_id,
        .summary = counter_summaries[i],
    };
    IREE_RETURN_IF_ERROR(
        loom_target_compile_report_record_wait_counter_row(report, &row));
  }
  for (uint32_t i = 0; i < LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT; ++i) {
    const uint32_t counter_id = i + 1;
    const iree_string_view_t counter_name =
        loom_amdgpu_wait_counter_name(counter_id);
    for (uint32_t reason_id = 0; reason_id < LOOM_AMDGPU_WAIT_PLAN_REASON_COUNT;
         ++reason_id) {
      if (reason_summaries[i][reason_id].action_count == 0) {
        continue;
      }
      const loom_target_compile_report_wait_reason_summary_row_t row = {
          .function_name = report->function_name,
          .counter_name = counter_name,
          .reason_name = loom_amdgpu_wait_plan_reason_name(
              (loom_amdgpu_wait_plan_reason_t)reason_id),
          .counter_id = counter_id,
          .reason_id = reason_id,
          .summary = reason_summaries[i][reason_id],
      };
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_record_wait_reason_summary_row(report,
                                                                    &row));
    }
  }
  if (!loom_target_compile_report_wants_details(
          report, LOOM_TARGET_COMPILE_REPORT_DETAIL_WAIT_PLAN)) {
    return iree_ok_status();
  }
  for (iree_host_size_t i = 0; i < wait_plan->action_count; ++i) {
    const loom_amdgpu_wait_plan_action_t* action = &wait_plan->actions[i];
    const uint32_t outstanding_after = action->target_count;
    const uint32_t drained_count =
        action->outstanding_before > outstanding_after
            ? action->outstanding_before - outstanding_after
            : 0;
    const loom_amdgpu_kernel_plan_wait_endpoint_t producer =
        loom_amdgpu_kernel_plan_wait_endpoint(wait_plan, action->producer_node);
    const loom_amdgpu_kernel_plan_wait_endpoint_t consumer =
        loom_amdgpu_kernel_plan_wait_endpoint(wait_plan, action->consumer_node);
    const loom_target_compile_report_wait_action_row_t row = {
        .function_name = report->function_name,
        .counter_name = loom_amdgpu_wait_counter_name(action->counter_id),
        .action_name = loom_amdgpu_kernel_plan_wait_action_name(action->kind),
        .reason_name = loom_amdgpu_wait_plan_reason_name(action->reason),
        .counter_id = action->counter_id,
        .action_id = (uint32_t)action->kind,
        .reason_id = (uint32_t)action->reason,
        .block_index = action->block_index,
        .node_index = action->node_index,
        .scheduled_ordinal = action->scheduled_ordinal,
        .producer_node = producer.node_index,
        .producer_scheduled_ordinal = producer.scheduled_ordinal,
        .producer_operation_name = producer.operation_name,
        .producer_descriptor_key = producer.descriptor_key,
        .producer_semantic_tag = producer.semantic_tag,
        .consumer_node = consumer.node_index,
        .consumer_scheduled_ordinal = consumer.scheduled_ordinal,
        .consumer_operation_name = consumer.operation_name,
        .consumer_descriptor_key = consumer.descriptor_key,
        .consumer_semantic_tag = consumer.semantic_tag,
        .target_count = action->target_count,
        .outstanding_before = action->outstanding_before,
        .outstanding_after = outstanding_after,
        .drained_count = drained_count,
    };
    IREE_RETURN_IF_ERROR(
        loom_target_compile_report_record_wait_action_row(report, &row));
  }
  return iree_ok_status();
}

static loom_target_compile_report_target_resources_t
loom_amdgpu_kernel_plan_target_resources(
    const loom_amdgpu_occupancy_target_resources_t* target_resources) {
  return (loom_target_compile_report_target_resources_t){
      .scalar_register_class = target_resources->scalar_register_class,
      .scalar_register_count = target_resources->scalar_register_count,
      .vector_register_class = target_resources->vector_register_class,
      .vector_register_count = target_resources->vector_register_count,
      .subgroup_size = target_resources->wave_size,
      .max_subgroups_per_simd = target_resources->max_waves_per_simd,
      .resident_subgroups_per_simd = target_resources->resident_waves_per_simd,
      .occupancy_percent = target_resources->occupancy_percent,
      .limiting_resource = target_resources->limiting_resource,
      .residency_summary = target_resources->residency_summary,
  };
}

static iree_status_t loom_amdgpu_kernel_plan_retain_direct_resource_units(
    const uint32_t* source_units, iree_host_size_t source_unit_count,
    const loom_target_residency_direct_resource_override_t* overrides,
    iree_host_size_t override_count, iree_arena_allocator_t* arena,
    const uint32_t** out_direct_resource_units) {
  *out_direct_resource_units = NULL;
  uint32_t* direct_resource_units = NULL;
  if (source_unit_count != 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        arena, source_unit_count, sizeof(*direct_resource_units),
        (void**)&direct_resource_units));
    memcpy(direct_resource_units, source_units,
           source_unit_count * sizeof(*direct_resource_units));
  }
  for (iree_host_size_t i = 0; i < override_count; ++i) {
    IREE_ASSERT_LT(overrides[i].resource_id, source_unit_count);
    direct_resource_units[overrides[i].resource_id] = overrides[i].units;
  }
  *out_direct_resource_units = direct_resource_units;
  return iree_ok_status();
}

IREE_ATTRIBUTE_NOINLINE IREE_ATTRIBUTE_COLD static iree_status_t
loom_amdgpu_kernel_plan_emit_unsatisfied_residency_requirement(
    const loom_low_emission_frame_t* frame,
    const loom_amdgpu_kernel_record_t* record,
    const loom_amdgpu_occupancy_target_resources_t* occupancy,
    iree_diagnostic_emitter_t emitter) {
  IREE_ASSERT(emitter.fn != NULL);
  IREE_ASSERT(iree_any_bit_set(
      occupancy->residency_summary.flags,
      LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_REQUIRED_TIER_UNSATISFIED));
  IREE_ASSERT(record->metadata.has_required_workgroup_size);
  IREE_ASSERT_NE(occupancy->waves_per_workgroup, 0);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(
          loom_low_diagnostic_target_key(&frame->schedule.target)),
      loom_param_string(
          loom_low_diagnostic_export_name(&frame->schedule.target)),
      loom_param_string(
          loom_low_diagnostic_config_key(&frame->schedule.target)),
      loom_param_string(loom_low_diagnostic_function_name(
          frame->module, frame->schedule.function_op)),
      loom_param_string(loom_low_diagnostic_operation_name(
          frame->module, frame->schedule.function_op)),
      loom_param_string(record->processor->name),
      loom_param_u32(record->metadata.max_flat_workgroup_size),
      loom_param_u32(occupancy->waves_per_workgroup),
      loom_param_u32(record->metadata.wavefront_size),
      loom_param_u32(occupancy->residency_summary.required_tier),
      loom_param_u32(occupancy->resident_waves_per_simd),
      loom_param_u32(record->metadata.sgpr_count),
      loom_param_u32(record->metadata.vgpr_count),
      loom_param_u32(record->metadata.group_segment_fixed_size),
      loom_param_string(occupancy->limiting_resource),
  };
  const loom_diagnostic_emission_t emission = {
      .op = frame->schedule.function_op,
      .error = LOOM_ERR_AMDGPU_052,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  return iree_diagnostic_emit(emitter, &emission);
}

// Finalizes target resource accounting before native emission. The allocation
// vector owns every descriptor register class; the prepared record supplies
// ABI and hardware-entry floors that are intentionally outside allocatable
// body storage. Only reports and rejected kernels retain a copied exact vector.
static iree_status_t loom_amdgpu_kernel_plan_admit_residency(
    const loom_low_emission_frame_t* frame,
    const loom_amdgpu_kernel_record_t* record,
    iree_diagnostic_emitter_t emitter, loom_target_compile_report_t* report,
    iree_arena_allocator_t* arena, bool* out_accepted) {
  *out_accepted = false;
  const uint32_t* allocation_direct_resource_units =
      frame->allocation.physical_extents.ends_by_reg_class;
  const iree_host_size_t direct_resource_unit_count =
      frame->allocation.physical_extents.count;
  IREE_ASSERT(frame->residency.model != NULL);
  IREE_ASSERT_EQ(direct_resource_unit_count,
                 frame->residency.model->direct_resources.resource_count);
  IREE_ASSERT_LT(LOOM_AMDGPU_REG_CLASS_ID_SGPR, direct_resource_unit_count);
  IREE_ASSERT_LT(LOOM_AMDGPU_REG_CLASS_ID_VGPR, direct_resource_unit_count);
  IREE_ASSERT_GE(
      record->metadata.sgpr_count,
      allocation_direct_resource_units[LOOM_AMDGPU_REG_CLASS_ID_SGPR]);
  IREE_ASSERT_GE(
      record->architectural_vgpr_count,
      allocation_direct_resource_units[LOOM_AMDGPU_REG_CLASS_ID_VGPR]);
  const loom_target_residency_direct_resource_override_t resource_overrides[] =
      {
          {
              .resource_id = LOOM_AMDGPU_REG_CLASS_ID_SGPR,
              .units = record->metadata.sgpr_count,
          },
          {
              .resource_id = LOOM_AMDGPU_REG_CLASS_ID_VGPR,
              .units = record->architectural_vgpr_count,
          },
      };
  const uint32_t final_residency_tier =
      loom_target_residency_evaluate_tier_with_direct_resource_overrides(
          frame->residency, allocation_direct_resource_units,
          direct_resource_unit_count, resource_overrides,
          IREE_ARRAYSIZE(resource_overrides));
  const bool accepted = loom_target_residency_view_meets_required_tier(
      frame->residency, final_residency_tier);
  if (accepted && report == NULL) {
    *out_accepted = true;
    return iree_ok_status();
  }

  const uint32_t* direct_resource_units = NULL;
  IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_plan_retain_direct_resource_units(
      allocation_direct_resource_units, direct_resource_unit_count,
      resource_overrides, IREE_ARRAYSIZE(resource_overrides), arena,
      &direct_resource_units));
  loom_target_residency_constraint_list_t residency_constraints = {0};
  const bool capture_residency_constraints =
      accepted &&
      loom_target_compile_report_wants_details(
          report, LOOM_TARGET_COMPILE_REPORT_DETAIL_RESIDENCY_CONSTRAINTS);
  loom_amdgpu_occupancy_target_resources_t occupancy = {0};
  const uint32_t flat_workgroup_size =
      record->metadata.has_required_workgroup_size
          ? record->metadata.max_flat_workgroup_size
          : 0;
  IREE_RETURN_IF_ERROR(loom_amdgpu_occupancy_build_target_resources(
      record->processor, record->metadata.wavefront_size, direct_resource_units,
      direct_resource_unit_count, flat_workgroup_size,
      record->metadata.group_segment_fixed_size, arena, &occupancy,
      capture_residency_constraints ? &residency_constraints : NULL));
  if (!accepted) {
    return loom_amdgpu_kernel_plan_emit_unsatisfied_residency_requirement(
        frame, record, &occupancy, emitter);
  }

  const loom_target_compile_report_target_resources_t target_resources =
      loom_amdgpu_kernel_plan_target_resources(&occupancy);
  loom_target_compile_report_record_target_resources(report, &target_resources);
  if (capture_residency_constraints) {
    IREE_RETURN_IF_ERROR(
        loom_target_compile_report_record_residency_constraints(
            report, &residency_constraints));
  }
  *out_accepted = true;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_kernel_plan_build(
    const loom_low_emission_frame_t* frame,
    const loom_amdgpu_kernel_plan_options_t* options,
    iree_arena_allocator_t* arena, bool* out_accepted,
    loom_amdgpu_kernel_plan_t* out_plan) {
  *out_accepted = false;
  *out_plan = (loom_amdgpu_kernel_plan_t){0};

  loom_amdgpu_native_preflight_t preflight = {0};
  const loom_amdgpu_native_preflight_options_t preflight_options = {
      .emitter = options->emitter,
  };
  IREE_RETURN_IF_ERROR(loom_amdgpu_native_preflight_analyze(
      &frame->schedule, &frame->allocation, &preflight_options, &preflight));
  if (preflight.error_count != 0) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_amdgpu_packet_plan_build(
      &frame->schedule, &frame->allocation, arena, &out_plan->packet_plan));
  const loom_amdgpu_kernel_record_options_t record_options = {
      .abi_layout = options->abi_layout,
      .abi_verify = options->abi_verify,
      .preflight = &preflight,
      .minimum_vgpr_count =
          out_plan->packet_plan.wait_states.minimum_vgpr_count,
  };
  IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_record_build(
      &frame->schedule, &frame->allocation, &record_options, &out_plan->record,
      arena));

  bool residency_accepted = false;
  IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_plan_admit_residency(
      frame, &out_plan->record, options->emitter, options->report, arena,
      &residency_accepted));
  if (!residency_accepted) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_amdgpu_kernel_plan_record_wait_plan(
      options->report, &out_plan->packet_plan));
  *out_accepted = true;
  return iree_ok_status();
}
