// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/sanitizer_ubsan_report.h"

#include "loom/codegen/low/builder.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/amdgpu/abi/feedback.h"
#include "loom/target/arch/amdgpu/lower/feedback.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/registers.h"

static bool loom_amdgpu_sanitizer_ubsan_check_kind_is_valid(
    loom_amdgpu_ubsan_check_kind_t check_kind) {
  switch (check_kind) {
    case LOOM_AMDGPU_UBSAN_CHECK_KIND_UNKNOWN:
    case LOOM_AMDGPU_UBSAN_CHECK_KIND_INTEGER_OVERFLOW:
    case LOOM_AMDGPU_UBSAN_CHECK_KIND_DIVIDE_BY_ZERO:
    case LOOM_AMDGPU_UBSAN_CHECK_KIND_ALIGNMENT:
    case LOOM_AMDGPU_UBSAN_CHECK_KIND_FLOAT_NAN_CONTRACT:
    case LOOM_AMDGPU_UBSAN_CHECK_KIND_UNREACHABLE:
    case LOOM_AMDGPU_UBSAN_CHECK_KIND_ASSERTION:
      return true;
    default:
      return false;
  }
}

static void loom_amdgpu_sanitizer_ubsan_require_data_register(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    loom_value_id_t value, uint32_t unit_count) {
  IREE_ASSERT(value < builder->module->values.count,
              "AMDGPU UBSAN report received an invalid low value");
  const loom_type_t type = loom_module_value_type(builder->module, value);
  IREE_ASSERT(loom_low_type_is_register(type) &&
                  loom_low_register_type_descriptor_set_stable_id(type) ==
                      descriptor_set->stable_id &&
                  loom_low_register_type_unit_count(type) == unit_count,
              "AMDGPU UBSAN report received a low value with an unsupported "
              "register shape");
  const uint16_t register_class = loom_low_register_type_class_id(type);
  IREE_ASSERT(register_class == LOOM_AMDGPU_REG_CLASS_ID_SGPR ||
                  register_class == LOOM_AMDGPU_REG_CLASS_ID_VGPR,
              "AMDGPU UBSAN report value must be an SGPR or VGPR");
}

static void loom_amdgpu_sanitizer_ubsan_require_vgpr(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    loom_value_id_t value, uint32_t unit_count) {
  loom_amdgpu_sanitizer_ubsan_require_data_register(builder, descriptor_set,
                                                    value, unit_count);
  const loom_type_t type = loom_module_value_type(builder->module, value);
  IREE_ASSERT_EQ(loom_low_register_type_class_id(type),
                 LOOM_AMDGPU_REG_CLASS_ID_VGPR);
}

static void loom_amdgpu_sanitizer_validate_ubsan_report(
    const loom_amdgpu_sanitizer_ubsan_report_t* report) {
  IREE_ASSERT(
      loom_amdgpu_sanitizer_ubsan_check_kind_is_valid(report->check_kind),
      "AMDGPU UBSAN report check kind is invalid");
  IREE_ASSERT_EQ(report->flags, LOOM_AMDGPU_UBSAN_REPORT_FLAG_NONE);
}

static void loom_amdgpu_sanitizer_validate_ubsan_report_values(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_sanitizer_ubsan_report_t* report) {
  loom_amdgpu_sanitizer_ubsan_require_data_register(builder, descriptor_set,
                                                    report->site_id, 2);
  loom_amdgpu_sanitizer_ubsan_require_data_register(builder, descriptor_set,
                                                    report->operand0, 2);
  loom_amdgpu_sanitizer_ubsan_require_data_register(builder, descriptor_set,
                                                    report->operand1, 2);
}

iree_status_t loom_amdgpu_build_sanitizer_ubsan_report_payload(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_feedback_packet_address_t* packet_address,
    const loom_amdgpu_sanitizer_ubsan_report_t* report,
    loom_location_id_t location) {
  loom_amdgpu_sanitizer_validate_ubsan_report(report);
  loom_amdgpu_sanitizer_validate_ubsan_report_values(builder, descriptor_set,
                                                     report);
  const uint32_t payload_base = LOOM_AMDGPU_FEEDBACK_PACKET_BYTE_LENGTH;
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_u32_constant(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_RECORD_LENGTH_OFFSET,
      LOOM_AMDGPU_UBSAN_REPORT_BYTE_LENGTH, location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_u32_constant(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_ABI_VERSION_OFFSET,
      LOOM_AMDGPU_UBSAN_REPORT_ABI_VERSION, location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_u32_constant(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_CHECK_KIND_OFFSET,
      report->check_kind, location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_u32_constant(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_FLAGS_OFFSET, report->flags,
      location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_b64(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_SITE_ID_OFFSET, report->site_id,
      location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_b64(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_OPERAND0_OFFSET, report->operand0,
      location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_b64(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_OPERAND1_OFFSET, report->operand1,
      location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_u64_constant(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_0_OFFSET, 0,
      location));
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_feedback_packet_store_u64_constant(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_1_OFFSET, 0,
      location));
  return loom_amdgpu_build_feedback_packet_store_u64_constant(
      builder, descriptor_set, packet_address,
      payload_base + LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_2_OFFSET, 0,
      location);
}

static iree_status_t loom_amdgpu_sanitizer_build_ubsan_payload_callback(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_feedback_packet_address_t* packet_address,
    const void* payload_context, loom_location_id_t location) {
  return loom_amdgpu_build_sanitizer_ubsan_report_payload(
      builder, descriptor_set, packet_address,
      (const loom_amdgpu_sanitizer_ubsan_report_t*)payload_context, location);
}

static iree_status_t
loom_amdgpu_sanitizer_build_ubsan_report_terminate_from_current_block(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    loom_symbol_ref_t feedback_config_symbol,
    const loom_amdgpu_feedback_packet_source_t* source,
    const loom_amdgpu_sanitizer_ubsan_report_t* report,
    loom_location_id_t location, loom_block_t** out_terminal_block) {
  loom_amdgpu_sanitizer_validate_ubsan_report(report);
  loom_amdgpu_sanitizer_validate_ubsan_report_values(builder, descriptor_set,
                                                     report);
  const loom_amdgpu_feedback_packet_producer_t producer = {
      .payload_byte_length = LOOM_AMDGPU_UBSAN_REPORT_BYTE_LENGTH,
      .packet_kind = LOOM_AMDGPU_FEEDBACK_PACKET_KIND_UBSAN,
      .packet_flags = LOOM_AMDGPU_FEEDBACK_PACKET_FLAG_ASYNC,
      .source = source,
      .build_payload = loom_amdgpu_sanitizer_build_ubsan_payload_callback,
      .payload_context = report,
  };
  return loom_amdgpu_build_feedback_packet_producer_terminate(
      builder, descriptor_set, feedback_config_symbol, &producer, location,
      out_terminal_block);
}

static iree_status_t loom_amdgpu_sanitizer_ubsan_define_vgpr_arg(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    loom_block_t* block, uint32_t unit_count, loom_value_id_t* out_value) {
  *out_value = LOOM_VALUE_ID_INVALID;
  loom_type_t type = loom_type_none();
  IREE_RETURN_IF_ERROR(loom_low_build_register_type(
      descriptor_set, LOOM_AMDGPU_REG_CLASS_ID_VGPR, unit_count, &type));
  return loom_builder_define_block_arg(builder, block, type, out_value);
}

static iree_status_t loom_amdgpu_sanitizer_define_ubsan_island_args(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    loom_block_t* entry_block, loom_amdgpu_ubsan_check_kind_t check_kind,
    loom_amdgpu_ubsan_report_flags_t flags,
    loom_amdgpu_sanitizer_ubsan_report_island_t* island) {
  IREE_RETURN_IF_ERROR(loom_amdgpu_sanitizer_ubsan_define_vgpr_arg(
      builder, descriptor_set, entry_block, 2,
      &island->source_args.dispatch_ptr));
  IREE_RETURN_IF_ERROR(loom_amdgpu_sanitizer_ubsan_define_vgpr_arg(
      builder, descriptor_set, entry_block, 1,
      &island->source_args.workgroup_id_x));
  IREE_RETURN_IF_ERROR(loom_amdgpu_sanitizer_ubsan_define_vgpr_arg(
      builder, descriptor_set, entry_block, 1,
      &island->source_args.workitem_id_x));
  island->report_args.check_kind = check_kind;
  island->report_args.flags = flags;
  IREE_RETURN_IF_ERROR(loom_amdgpu_sanitizer_ubsan_define_vgpr_arg(
      builder, descriptor_set, entry_block, 2, &island->report_args.site_id));
  IREE_RETURN_IF_ERROR(loom_amdgpu_sanitizer_ubsan_define_vgpr_arg(
      builder, descriptor_set, entry_block, 2, &island->report_args.operand0));
  return loom_amdgpu_sanitizer_ubsan_define_vgpr_arg(
      builder, descriptor_set, entry_block, 2, &island->report_args.operand1);
}

iree_status_t loom_amdgpu_build_sanitizer_ubsan_report_island(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    loom_block_t* after_block, loom_symbol_ref_t feedback_config_symbol,
    loom_amdgpu_ubsan_check_kind_t check_kind,
    loom_amdgpu_ubsan_report_flags_t flags, loom_location_id_t location,
    loom_amdgpu_sanitizer_ubsan_report_island_t* out_island) {
  IREE_ASSERT_ARGUMENT(out_island);
  *out_island = (loom_amdgpu_sanitizer_ubsan_report_island_t){0};
  const loom_amdgpu_sanitizer_ubsan_report_t report = {
      .check_kind = check_kind,
      .flags = flags,
  };
  loom_amdgpu_sanitizer_validate_ubsan_report(&report);
  IREE_ASSERT(after_block->parent_region != NULL,
              "AMDGPU UBSAN report island requires a low region block");

  loom_amdgpu_sanitizer_ubsan_report_island_t island = {
      .check_kind = check_kind,
      .flags = flags,
  };
  IREE_RETURN_IF_ERROR(loom_region_insert_block(
      builder->module, after_block->parent_region,
      (uint16_t)(after_block->region_index + 1), &island.entry_block));
  IREE_RETURN_IF_ERROR(loom_amdgpu_sanitizer_define_ubsan_island_args(
      builder, descriptor_set, island.entry_block, check_kind, flags, &island));
  loom_builder_set_block(builder, island.entry_block);
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_sanitizer_build_ubsan_report_terminate_from_current_block(
          builder, descriptor_set, feedback_config_symbol, &island.source_args,
          &island.report_args, location, &island.terminal_block));

  *out_island = island;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_build_sanitizer_ubsan_report_branch(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_sanitizer_ubsan_report_island_t* island,
    const loom_amdgpu_feedback_packet_source_t* source,
    const loom_amdgpu_sanitizer_ubsan_report_t* report,
    loom_location_id_t location) {
  loom_amdgpu_sanitizer_validate_ubsan_report(report);
  IREE_ASSERT(report->check_kind == island->check_kind &&
                  report->flags == island->flags,
              "AMDGPU UBSAN report does not match its island");
  IREE_ASSERT(builder->ip.before_op == NULL,
              "AMDGPU UBSAN report branch must be built at the end of a low "
              "block");
  const loom_value_id_t args[] = {
      source->dispatch_ptr, source->workgroup_id_x, source->workitem_id_x,
      report->site_id,      report->operand0,       report->operand1,
  };
  static const uint8_t arg_unit_counts[] = {2, 1, 1, 2, 2, 2};
  IREE_ASSERT_EQ(IREE_ARRAYSIZE(args), IREE_ARRAYSIZE(arg_unit_counts));
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(args); ++i) {
    loom_amdgpu_sanitizer_ubsan_require_vgpr(builder, descriptor_set, args[i],
                                             arg_unit_counts[i]);
  }
  loom_op_t* branch_op = NULL;
  return loom_low_br_build(builder, island->entry_block, args,
                           IREE_ARRAYSIZE(args), location, &branch_op);
}
