// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/reporting/loop_pipeline.h"

#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"
#include "loom/target/function_version.h"
#include "loom/target/reporting/report.h"
#include "loom/target/reporting/row_list.h"

iree_status_t loom_target_compile_report_record_loop_pipelines(
    loom_target_compile_report_t* report, const loom_module_t* module,
    const loom_function_version_list_t* versions) {
  if (!report || !versions) {
    return iree_ok_status();
  }
  for (iree_host_size_t i = 0; i < versions->count; ++i) {
    const loom_target_function_version_t* version =
        loom_target_function_version_const_cast(versions->values[i]);
    if (!version || !version->loop_pipelines.head) {
      continue;
    }
    const loom_symbol_ref_t function_ref =
        loom_func_like_callee(version->base.function);
    const iree_string_view_t function_name = loom_string_table_get(
        &module->strings,
        module->symbols.entries[function_ref.symbol_id].name_id);
    report->detail_flags |= LOOM_TARGET_COMPILE_REPORT_DETAIL_SOURCE_LOW_ROWS;
    for (const loom_source_loop_pipeline_t* pipeline =
             version->loop_pipelines.head;
         pipeline; pipeline = pipeline->next) {
      const loom_target_compile_report_loop_pipeline_row_t row = {
          .function_name = function_name,
          .loop_ordinal = pipeline->loop_ordinal,
          .depth = pipeline->depth,
          .values_per_record = pipeline->values_per_record,
          .read_count = pipeline->read_count,
      };
      IREE_RETURN_IF_ERROR(loom_target_compile_report_row_list_append(
          &report->loop_pipeline_rows, sizeof(row), report->allocator, &row));
      if (!loom_target_compile_report_wants_details(
              report, LOOM_TARGET_COMPILE_REPORT_DETAIL_SOURCE_LOW_ROWS)) {
        continue;
      }
      for (uint32_t j = 0; j < pipeline->operation_count; ++j) {
        const loom_source_loop_pipeline_operation_t* operation =
            &pipeline->operations[j];
        const loom_target_compile_report_loop_pipeline_stage_row_t stage = {
            .function_name = function_name,
            .loop_ordinal = pipeline->loop_ordinal,
            .position = operation->source_position,
            .op_name = operation->op_name,
            .iteration_lookahead = operation->iteration_lookahead,
            .partition = operation->partition,
        };
        IREE_RETURN_IF_ERROR(loom_target_compile_report_row_list_append(
            &report->loop_pipeline_stage_rows, sizeof(stage), report->allocator,
            &stage));
      }
    }
  }
  return iree_ok_status();
}
