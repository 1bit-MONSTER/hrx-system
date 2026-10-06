// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/spirv/lower/workgroup.h"

#include <stdint.h>

#include "loom/analysis/storage_layout.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/facts.h"
#include "loom/ir/module.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/spirv/lower/workgroup_layout.h"
#include "loom/util/fact_table.h"

typedef enum loom_spirv_workgroup_plan_kind_e {
  LOOM_SPIRV_WORKGROUP_PLAN_ALLOCA = 1,
  LOOM_SPIRV_WORKGROUP_PLAN_VIEW = 2,
} loom_spirv_workgroup_plan_kind_t;

typedef struct loom_spirv_workgroup_alloca_plan_t {
  // Static byte length of the Workgroup allocation.
  int64_t byte_length;
  // Static byte alignment of the Workgroup allocation.
  int64_t byte_alignment;
  // Whether the allocation uses its scalar carrier's shared physical arena.
  bool packed;
} loom_spirv_workgroup_alloca_plan_t;

typedef struct loom_spirv_workgroup_view_plan_t {
  // Source storage-root value that has already lowered to low.storage.
  loom_value_id_t root_value_id;
  // Register class used for the Workgroup array base pointer.
  uint16_t array_pointer_reg_class_id;
} loom_spirv_workgroup_view_plan_t;

static bool loom_spirv_workgroup_i64_is_power_of_two(int64_t value) {
  return value > 0 && value <= UINT32_MAX &&
         iree_math_is_power_of_two_i64(value);
}

static iree_status_t loom_spirv_workgroup_emit_rejected(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    iree_string_view_t field_name, loom_type_t type,
    loom_spirv_workgroup_plan_kind_t plan_kind,
    loom_low_lower_plan_t* out_plan) {
  *out_plan = loom_low_lower_plan_make(plan_kind, NULL);
  return loom_low_lower_emit_source_type_unsupported(context, source_op,
                                                     field_name, type);
}

static bool loom_spirv_workgroup_view_reference(
    const loom_value_fact_table_t* fact_table, loom_value_id_t value_id,
    loom_value_fact_view_reference_t* out_reference) {
  *out_reference = (loom_value_fact_view_reference_t){0};
  if (!loom_value_facts_query_view_reference(
          &fact_table->context,
          loom_value_fact_table_lookup(fact_table, value_id), out_reference)) {
    return false;
  }
  out_reference->root_value_id =
      loom_value_fact_view_reference_resolve_root_value(*out_reference,
                                                        value_id);
  return out_reference->memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP;
}

static iree_status_t loom_spirv_workgroup_view_plan_from_facts(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_spirv_workgroup_view_plan_t* out_plan, bool* out_selected,
    bool* out_has_workgroup_facts) {
  *out_plan = (loom_spirv_workgroup_view_plan_t){0};
  *out_selected = false;
  *out_has_workgroup_facts = false;
  const loom_value_id_t result = loom_buffer_view_result(source_op);
  IREE_RETURN_IF_ERROR(loom_spirv_resolve_workgroup_view_reg_class(
      context, result, out_has_workgroup_facts,
      &out_plan->array_pointer_reg_class_id));
  if (!*out_has_workgroup_facts ||
      out_plan->array_pointer_reg_class_id == LOOM_LOW_REG_CLASS_NONE) {
    return iree_ok_status();
  }
  loom_value_fact_view_reference_t reference = {0};
  if (!loom_spirv_workgroup_view_reference(
          loom_low_lower_context_fact_table(context), result, &reference)) {
    return iree_make_status(
        IREE_STATUS_INTERNAL,
        "selected SPIR-V Workgroup view has no Workgroup reference");
  }
  out_plan->root_value_id = reference.root_value_id;
  *out_selected = true;
  return iree_ok_status();
}

static iree_status_t loom_spirv_select_workgroup_alloca(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t* out_plan) {
  if (loom_buffer_alloca_memory_space(source_op) !=
      LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP) {
    return iree_ok_status();
  }
  loom_spirv_workgroup_alloca_plan_t plan = {0};
  const loom_value_fact_table_t* fact_table =
      loom_low_lower_context_fact_table(context);
  if (!loom_value_facts_as_non_negative_i64_maximum(
          loom_value_fact_table_lookup(
              fact_table, loom_buffer_alloca_byte_length(source_op)),
          &plan.byte_length) ||
      plan.byte_length <= 0) {
    *out_plan =
        loom_low_lower_plan_make(LOOM_SPIRV_WORKGROUP_PLAN_ALLOCA, NULL);
    return loom_low_lower_emit_function_storage_extent_unsupported(
        context, source_op, LOOM_STORAGE_SPACE_WORKGROUP,
        loom_buffer_alloca_byte_length(source_op));
  }
  if (!loom_spirv_workgroup_i64_is_power_of_two(
          loom_buffer_alloca_base_alignment(source_op))) {
    return loom_spirv_workgroup_emit_rejected(
        context, source_op, IREE_SV("workgroup_storage"),
        loom_module_value_type(loom_low_lower_context_module(context),
                               loom_buffer_alloca_result(source_op)),
        LOOM_SPIRV_WORKGROUP_PLAN_ALLOCA, out_plan);
  }
  plan.byte_alignment = loom_buffer_alloca_base_alignment(source_op);
  IREE_RETURN_IF_ERROR(loom_spirv_workgroup_layout_record_alloca(
      context, source_op, (uint64_t)plan.byte_length,
      (uint64_t)plan.byte_alignment, &plan.packed));

  loom_spirv_workgroup_alloca_plan_t* plan_data = NULL;
  IREE_RETURN_IF_ERROR(loom_low_lower_allocate_plan_data(
      context, sizeof(*plan_data), (void**)&plan_data));
  *plan_data = plan;
  *out_plan =
      loom_low_lower_plan_make(LOOM_SPIRV_WORKGROUP_PLAN_ALLOCA, plan_data);
  return iree_ok_status();
}

static iree_status_t loom_spirv_select_workgroup_view(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t* out_plan) {
  loom_spirv_workgroup_view_plan_t plan = {0};
  bool selected = false;
  bool has_workgroup_facts = false;
  IREE_RETURN_IF_ERROR(loom_spirv_workgroup_view_plan_from_facts(
      context, source_op, &plan, &selected, &has_workgroup_facts));
  if (!selected) {
    if (has_workgroup_facts) {
      return loom_spirv_workgroup_emit_rejected(
          context, source_op, IREE_SV("workgroup_view"),
          loom_module_value_type(loom_low_lower_context_module(context),
                                 loom_buffer_view_result(source_op)),
          LOOM_SPIRV_WORKGROUP_PLAN_VIEW, out_plan);
    }
    return iree_ok_status();
  }
  loom_spirv_workgroup_view_plan_t* plan_data = NULL;
  IREE_RETURN_IF_ERROR(loom_low_lower_allocate_plan_data(
      context, sizeof(*plan_data), (void**)&plan_data));
  *plan_data = plan;
  *out_plan =
      loom_low_lower_plan_make(LOOM_SPIRV_WORKGROUP_PLAN_VIEW, plan_data);
  return iree_ok_status();
}

iree_status_t loom_spirv_select_workgroup_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t* out_plan) {
  *out_plan = loom_low_lower_plan_empty();
  switch (source_op->kind) {
    case LOOM_OP_BUFFER_ALLOCA:
      return loom_spirv_select_workgroup_alloca(context, source_op, out_plan);
    case LOOM_OP_BUFFER_VIEW:
      return loom_spirv_select_workgroup_view(context, source_op, out_plan);
    default:
      return iree_ok_status();
  }
}

void loom_spirv_mark_workgroup_plan_storage_demands(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan) {
  (void)source_op;
  switch ((loom_spirv_workgroup_plan_kind_t)plan.id) {
    case LOOM_SPIRV_WORKGROUP_PLAN_ALLOCA:
      return;
    case LOOM_SPIRV_WORKGROUP_PLAN_VIEW: {
      const loom_spirv_workgroup_view_plan_t* view_plan =
          (const loom_spirv_workgroup_view_plan_t*)plan.target_data;
      loom_low_lower_require_source_value_storage(context,
                                                  view_plan->root_value_id);
      return;
    }
  }
  IREE_ASSERT_UNREACHABLE("SPIR-V Workgroup plan selected unknown op kind");
}

static iree_status_t loom_spirv_workgroup_measure_storage(
    loom_low_lower_context_t* context, uint64_t* out_byte_extent) {
  *out_byte_extent = 0;
  const iree_host_size_t storage_root_count =
      loom_spirv_workgroup_layout_storage_root_count(context);
  for (iree_host_size_t i = 0; i < storage_root_count; ++i) {
    const loom_spirv_workgroup_storage_root_requirement_t requirement =
        loom_spirv_workgroup_layout_storage_root_requirement(context, i);
    IREE_RETURN_IF_ERROR(loom_storage_layout_append(
        requirement.byte_length, requirement.byte_alignment, out_byte_extent,
        /*out_byte_offset=*/NULL));
  }

  const iree_host_size_t selected_plan_count =
      loom_low_lower_context_selected_plan_count(context);
  for (iree_host_size_t i = 0; i < selected_plan_count; ++i) {
    const loom_low_lower_selected_plan_view_t selected_plan =
        loom_low_lower_context_selected_plan_view(context, i);
    if (selected_plan.elided ||
        selected_plan.plan.id != LOOM_SPIRV_WORKGROUP_PLAN_ALLOCA) {
      continue;
    }
    const loom_spirv_workgroup_alloca_plan_t* alloca_plan =
        (const loom_spirv_workgroup_alloca_plan_t*)
            selected_plan.plan.target_data;
    IREE_ASSERT(alloca_plan != NULL);
    if (alloca_plan->packed) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_storage_layout_append(
        (uint64_t)alloca_plan->byte_length,
        (uint64_t)alloca_plan->byte_alignment, out_byte_extent,
        /*out_byte_offset=*/NULL));
  }
  return iree_ok_status();
}

iree_status_t loom_spirv_emit_workgroup_entry_setup(
    loom_low_lower_context_t* context) {
  uint64_t byte_extent = 0;
  IREE_RETURN_IF_ERROR(
      loom_spirv_workgroup_measure_storage(context, &byte_extent));
  const uint64_t byte_limit = loom_low_lower_context_bundle(context)
                                  ->snapshot->max_workgroup_storage_bytes;
  if (byte_limit != 0 && byte_extent > byte_limit) {
    const loom_diagnostic_param_t params[] = {
        loom_param_string(loom_low_lower_context_function_name(context)),
        loom_param_string(loom_low_lower_context_target_key(context)),
        loom_param_u64(byte_extent),
        loom_param_u64(byte_limit),
    };
    return loom_low_lower_emit_error_ref(
        context, loom_low_lower_context_source_function(context).op,
        LOOM_ERR_TARGET_051_REF, params, IREE_ARRAYSIZE(params));
  }
  return loom_spirv_workgroup_layout_emit_storage_roots(context);
}

static iree_status_t loom_spirv_lower_workgroup_alloca(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_spirv_workgroup_alloca_plan_t* plan) {
  if (plan->packed) {
    loom_value_id_t low_storage = LOOM_VALUE_ID_INVALID;
    loom_spirv_workgroup_layout_lookup_low_storage(
        context, loom_buffer_alloca_result(source_op), &low_storage);
    return loom_low_lower_bind_value(
        context, loom_buffer_alloca_result(source_op), low_storage);
  }
  loom_op_t* storage_op = NULL;
  IREE_RETURN_IF_ERROR(loom_low_storage_reserve_build(
      loom_low_lower_context_builder(context), plan->byte_length,
      plan->byte_alignment, loom_type_storage(LOOM_STORAGE_SPACE_WORKGROUP),
      source_op->location, &storage_op));
  return loom_low_lower_bind_value(
      context, loom_buffer_alloca_result(source_op),
      loom_low_storage_reserve_storage(storage_op));
}

static iree_status_t loom_spirv_lower_workgroup_view(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_spirv_workgroup_view_plan_t* plan) {
  loom_value_id_t low_storage = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_lookup_value(context, plan->root_value_id, &low_storage));
  loom_type_t array_pointer_type = loom_type_none();
  IREE_RETURN_IF_ERROR(loom_low_lower_make_register_type(
      context, plan->array_pointer_reg_class_id, 1, &array_pointer_type));
  loom_op_t* address_op = NULL;
  IREE_RETURN_IF_ERROR(loom_low_storage_address_build(
      loom_low_lower_context_builder(context), low_storage, /*offset=*/0,
      array_pointer_type, source_op->location, &address_op));
  return loom_low_lower_bind_value(context, loom_buffer_view_result(source_op),
                                   loom_low_storage_address_result(address_op));
}

iree_status_t loom_spirv_lower_workgroup_op(loom_low_lower_context_t* context,
                                            const loom_op_t* source_op,
                                            loom_low_lower_plan_t plan) {
  switch ((loom_spirv_workgroup_plan_kind_t)plan.id) {
    case LOOM_SPIRV_WORKGROUP_PLAN_ALLOCA:
      return loom_spirv_lower_workgroup_alloca(
          context, source_op,
          (const loom_spirv_workgroup_alloca_plan_t*)plan.target_data);
    case LOOM_SPIRV_WORKGROUP_PLAN_VIEW:
      return loom_spirv_lower_workgroup_view(
          context, source_op,
          (const loom_spirv_workgroup_view_plan_t*)plan.target_data);
  }
  IREE_ASSERT_UNREACHABLE("SPIR-V Workgroup plan selected unknown op kind");
  IREE_BUILTIN_UNREACHABLE();
}
