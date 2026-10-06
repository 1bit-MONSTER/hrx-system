// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/loom-check/analysis.h"

#include "loom/analysis/liveness.h"
#include "loom/analysis/liveness_json.h"
#include "loom/analysis/storage_interference.h"
#include "loom/codegen/low/diagnostics.h"
#include "loom/ir/module.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/type_registry.h"
#include "loom/util/fact_table.h"

iree_status_t loom_check_emit_liveness(loom_module_t* module,
                                       loom_func_like_t function,
                                       iree_arena_allocator_t* arena,
                                       iree_string_builder_t* output) {
  loom_liveness_analysis_t analysis = {0};
  IREE_RETURN_IF_ERROR(loom_liveness_analyze_region(
      module, loom_func_like_body(function), arena, &analysis));
  return loom_liveness_format_json(&analysis, NULL, output);
}

iree_status_t loom_check_emit_storage_interference(
    loom_module_t* module, loom_func_like_t function,
    iree_arena_allocator_t* arena, iree_string_builder_t* output) {
  loom_value_fact_table_t facts;
  IREE_RETURN_IF_ERROR(
      loom_value_fact_table_initialize(&facts, arena, module->values.count));
  loom_type_registry_configure_fact_context(&facts.context);
  IREE_RETURN_IF_ERROR(loom_value_fact_table_compute(&facts, module, function));
  loom_local_value_domain_t domain;
  IREE_RETURN_IF_ERROR(loom_local_value_domain_acquire_for_region_tree(
      module, loom_func_like_body(function), arena, &domain));
  loom_storage_interference_t* analysis = NULL;
  loom_value_id_t* roots = NULL;
  iree_status_t status = iree_arena_allocate_array(
      arena, domain.definition_count, sizeof(*roots), (void**)&roots);
  if (iree_status_is_ok(status)) {
    status = loom_storage_interference_analyze_function(
        module, &facts, &domain, function, arena, &analysis);
  }
  iree_host_size_t root_count = 0;
  for (loom_value_ordinal_t i = 0;
       i < domain.definition_count && iree_status_is_ok(status); ++i) {
    const loom_value_id_t value_id = domain.value_ids[i];
    const loom_value_t* value = loom_module_value(module, value_id);
    if (loom_value_is_block_arg(value) ||
        !loom_buffer_alloca_isa(loom_value_def_op(value))) {
      continue;
    }
    roots[root_count++] = value_id;
    const iree_string_view_t name =
        loom_low_diagnostic_value_name(module, value_id);
    status = iree_string_builder_append_format(
        output, "%.*s: accessed=%s single_instance=%s\n", (int)name.size,
        name.data,
        loom_storage_interference_root_may_be_accessed(analysis, value_id)
            ? "yes"
            : "no",
        loom_storage_interference_root_has_single_live_instance(analysis,
                                                                value_id)
            ? "yes"
            : "unproven");
  }
  for (iree_host_size_t i = 0; i < root_count && iree_status_is_ok(status);
       ++i) {
    for (iree_host_size_t j = i + 1;
         j < root_count && iree_status_is_ok(status); ++j) {
      bool nonoverlap = false;
      status = loom_storage_interference_prove_workgroup_nonoverlap(
          analysis, roots[i], roots[j], &nonoverlap);
      if (iree_status_is_ok(status)) {
        const iree_string_view_t lhs =
            loom_low_diagnostic_value_name(module, roots[i]);
        const iree_string_view_t rhs =
            loom_low_diagnostic_value_name(module, roots[j]);
        status = iree_string_builder_append_format(
            output, "%.*s / %.*s: workgroup_nonoverlap=%s\n", (int)lhs.size,
            lhs.data, (int)rhs.size, rhs.data, nonoverlap ? "yes" : "unproven");
      }
    }
  }
  loom_local_value_domain_release(&domain);
  return status;
}
