// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/function_storage.h"

#include "loom/codegen/low/lower/context.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/low/ops.h"

iree_status_t loom_low_lower_function_storage_select(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    bool* out_selected,
    const loom_low_lower_function_storage_plan_t** out_plan) {
  *out_selected = false;
  *out_plan = NULL;
  const loom_low_lower_function_storage_config_t* config =
      &context->policy->function_storage;
  const loom_value_fact_memory_space_t memory_space =
      loom_buffer_alloca_memory_space(source_op);
  const loom_low_lower_function_storage_mapping_t* mapping = NULL;
  for (iree_host_size_t i = 0; i < config->count; ++i) {
    if (config->mappings[i].memory_space == memory_space) {
      mapping = &config->mappings[i];
      break;
    }
  }
  if (!mapping) {
    return iree_ok_status();
  }
  *out_selected = true;

  int64_t byte_length = 0;
  if (!loom_value_facts_as_non_negative_i64_maximum(
          loom_value_fact_table_lookup(
              loom_low_lower_context_fact_table(context),
              loom_buffer_alloca_byte_length(source_op)),
          &byte_length) ||
      byte_length <= 0) {
    return loom_low_lower_emit_function_storage_extent_unsupported(
        context, source_op, mapping->storage_space,
        loom_buffer_alloca_byte_length(source_op));
  }
  loom_low_lower_function_storage_plan_t* plan = NULL;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_allocate_plan_data(context, sizeof(*plan), (void**)&plan));
  *plan = (loom_low_lower_function_storage_plan_t){
      .byte_length = byte_length,
      .byte_alignment = loom_buffer_alloca_base_alignment(source_op),
      .storage_space = mapping->storage_space,
  };
  IREE_RETURN_IF_ERROR(loom_low_lower_make_register_type(
      context, mapping->address_register_class, 1, &plan->address_type));
  *out_plan = plan;
  return iree_ok_status();
}

iree_status_t loom_low_lower_function_storage_emit(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_lower_function_storage_plan_t* plan) {
  loom_builder_t* builder = loom_low_lower_context_builder(context);
  loom_op_t* storage_op = NULL;
  IREE_RETURN_IF_ERROR(loom_low_storage_reserve_build(
      builder, plan->byte_length, plan->byte_alignment,
      loom_type_storage(plan->storage_space), source_op->location,
      &storage_op));
  loom_op_t* address_op = NULL;
  IREE_RETURN_IF_ERROR(loom_low_storage_address_build(
      builder, loom_low_storage_reserve_storage(storage_op), /*offset=*/0,
      plan->address_type, source_op->location, &address_op));
  return loom_low_lower_bind_value(context,
                                   loom_buffer_alloca_result(source_op),
                                   loom_low_storage_address_result(address_op));
}
