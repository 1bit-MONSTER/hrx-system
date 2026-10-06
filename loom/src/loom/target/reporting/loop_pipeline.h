// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Applied source loop schedules exposed by compile reports.

#ifndef LOOM_TARGET_REPORTING_LOOP_PIPELINE_H_
#define LOOM_TARGET_REPORTING_LOOP_PIPELINE_H_

#include "loom/ir/function_version.h"
#include "loom/target/loop_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_target_compile_report_t loom_target_compile_report_t;

typedef struct loom_target_compile_report_loop_pipeline_row_t {
  // Compiled function containing the applied source policy.
  iree_string_view_t function_name;
  // Stable ordinal among policies applied to this function version.
  iree_host_size_t loop_ordinal;
  // Requested iteration depth; one records a serial policy.
  uint32_t depth;
  // Number of SSA values in each of the depth-1 queued iteration records.
  uint32_t values_per_record;
  // Ordinary source read operations selected for the producer stage.
  uint32_t read_count;
} loom_target_compile_report_loop_pipeline_row_t;

typedef struct loom_target_compile_report_loop_pipeline_stage_row_t {
  // Compiled function containing the applied source policy.
  iree_string_view_t function_name;
  // Stable policy ordinal in this function version.
  iree_host_size_t loop_ordinal;
  // Operation position in the original source loop body.
  uint32_t position;
  // Source operation mnemonic borrowed from the compilation context.
  iree_string_view_t op_name;
  // Original iterations ahead of the ordered consumer; zero for consumers.
  uint32_t iteration_lookahead;
  // Interior partition used to place this operation copy.
  loom_source_loop_pipeline_partition_t partition;
} loom_target_compile_report_loop_pipeline_stage_row_t;

// Copies retained schedules from the compiled versions into report-owned rows.
// Called once at the pipeline/emit report boundary, after version production.
// The supplied list is the source of schedule facts; no IR traversal occurs.
// Strings borrow module/context storage under the ordinary report lifetime.
iree_status_t loom_target_compile_report_record_loop_pipelines(
    loom_target_compile_report_t* report, const loom_module_t* module,
    const loom_function_version_list_t* versions);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_REPORTING_LOOP_PIPELINE_H_
