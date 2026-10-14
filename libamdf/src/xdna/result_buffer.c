// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/result_buffer.h"

#include "libamdf/src/memory_resource.h"
#include "libamdf/src/structure.h"
#include "libamdf/src/xdna/context.h"
#include "libamdf/src/xdna/umd/result_buffer.h"

amdf_status_t AMDF_CALL amdf_xdna_context_query_result_buffer_info(
    amdf_xdna_context_t* context, amdf_xdna_result_buffer_info_t* out_info) {
  if (context == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  amdf_status_t status = amdf_structure_validate_output(
      out_info, AMDF_STRUCTURE_TYPE_XDNA_RESULT_BUFFER_INFO, sizeof(*out_info));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  amdf_xdna_result_buffer_info_t info = {0};
  status = amdf_xdna_umd_context_query_result_buffer_info(
      amdf_xdna_context_get_umd(context), &info);
  if (amdf_status_is_ok(status)) {
    info.type = out_info->type;
    info.structure_size = out_info->structure_size;
    info.next = out_info->next;
    info.memory_scope = amdf_xdna_context_get_memory_scope(context);
    info.timer_clock.counter_bit_count = 64;
    *out_info = info;
  }
  return status;
}

static amdf_status_t amdf_xdna_context_result_buffer(
    amdf_xdna_context_t* context,
    const amdf_xdna_result_buffer_binding_t* binding,
    amdf_xdna_result_buffer_operation_t operation) {
  if (context == NULL || binding == NULL || binding->memory == NULL ||
      binding->reserved != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_memory_t* memory = binding->memory;
  if (memory->scope != amdf_xdna_context_get_memory_scope(context)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (binding->access_ordinal >= memory->info.access_count ||
      binding->byte_length == 0 ||
      binding->byte_offset > memory->info.byte_length ||
      binding->byte_length > memory->info.byte_length - binding->byte_offset) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  const amdf_memory_access_state_t* access =
      &memory->accesses[binding->access_ordinal];
  const amdf_memory_access_t required_access =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  if (access->device != amdf_xdna_context_get_device(context) ||
      (access->info.access & required_access) != required_access) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_xdna_result_buffer_info_t info = {0};
  const amdf_status_t status = amdf_xdna_umd_context_query_result_buffer_info(
      amdf_xdna_context_get_umd(context), &info);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  if (binding->byte_length < info.minimum_byte_length ||
      binding->byte_length > info.maximum_byte_length ||
      binding->byte_offset % info.byte_offset_alignment != 0 ||
      binding->byte_length % info.byte_length_granularity != 0 ||
      (info.binding_mode == AMDF_XDNA_RESULT_BUFFER_BINDING_WHOLE_MEMORY &&
       (binding->byte_offset != 0 ||
        binding->byte_length != memory->info.byte_length))) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  return amdf_xdna_umd_context_result_buffer(
      amdf_xdna_context_get_umd(context), operation,
      memory->accesses[access->native_owner_ordinal].native,
      binding->byte_offset, binding->byte_length);
}

amdf_status_t AMDF_CALL amdf_xdna_context_attach_result_buffer(
    amdf_xdna_context_t* context,
    const amdf_xdna_result_buffer_binding_t* binding) {
  return amdf_xdna_context_result_buffer(
      context, binding, AMDF_XDNA_RESULT_BUFFER_OPERATION_ATTACH);
}

amdf_status_t AMDF_CALL amdf_xdna_context_collect_result_buffer(
    amdf_xdna_context_t* context,
    const amdf_xdna_result_buffer_binding_t* binding) {
  return amdf_xdna_context_result_buffer(
      context, binding, AMDF_XDNA_RESULT_BUFFER_OPERATION_COLLECT);
}

amdf_status_t AMDF_CALL amdf_xdna_context_detach_result_buffer(
    amdf_xdna_context_t* context,
    const amdf_xdna_result_buffer_binding_t* binding) {
  return amdf_xdna_context_result_buffer(
      context, binding, AMDF_XDNA_RESULT_BUFFER_OPERATION_DETACH);
}
