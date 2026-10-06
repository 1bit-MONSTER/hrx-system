// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Prepared check.scenario benchmark execution.

#ifndef LOOM_TOOLS_IREE_BENCHMARK_LOOM_SCENARIO_EXECUTION_H_
#define LOOM_TOOLS_IREE_BENCHMARK_LOOM_SCENARIO_EXECUTION_H_

#include "iree/base/api.h"
#include "loom/tools/iree-benchmark-loom/work_execution.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_benchmark_loom_scenario_execution_t
    iree_benchmark_loom_scenario_execution_t;

// Creates lazily prepared scenario benchmark execution state.
iree_status_t iree_benchmark_loom_scenario_execution_create(
    const iree_benchmark_loom_work_plan_execution_options_t* options,
    iree_benchmark_loom_scenario_execution_t** out_execution);

// Releases all prepared products and materialized values owned by |execution|.
void iree_benchmark_loom_scenario_execution_destroy(
    iree_benchmark_loom_scenario_execution_t* execution);

// Measures one concrete scenario trial and emits every logical result alias.
iree_status_t iree_benchmark_loom_run_scenario_work_item(
    iree_benchmark_loom_scenario_execution_t* execution,
    const iree_benchmark_loom_work_item_t* work_item,
    iree_host_size_t* inout_failed_benchmark_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_IREE_BENCHMARK_LOOM_SCENARIO_EXECUTION_H_
