// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared driver for composed boundary projection rule sets.

#ifndef LOOM_TRANSFORMS_BOUNDARY_PROJECTION_DRIVER_H_
#define LOOM_TRANSFORMS_BOUNDARY_PROJECTION_DRIVER_H_

#include "loom/pass/types.h"
#include "loom/target/function_version.h"
#include "loom/transforms/boundary/projection_rule.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_boundary_projection_statistics_t {
  // Function signatures rewritten by the topology adapter.
  int64_t functions_rewritten;
  // Semantic calls rewritten by the topology adapter.
  int64_t calls_rewritten;
  // Function returns rewritten by the topology adapter.
  int64_t returns_rewritten;
  // Direct CFG successor payloads rewritten by the topology adapter.
  int64_t cfg_edges_rewritten;
  // LoopLike operations rewritten by the topology adapter.
  int64_t loops_rewritten;
  // LoopLike operations inspected for recurrence projections.
  int64_t loops_checked;
  // Per-rule statistics parallel to the supplied rule list. The data is owned
  // by pass scratch storage and remains valid for the pass invocation.
  const loom_boundary_projection_rule_statistics_t* rules;
  // Number of entries in rules.
  iree_host_size_t rule_count;
} loom_boundary_projection_statistics_t;

// Optional cold consumer of a finalized, non-mutating projection plan.
// Invoked after all connected rejection propagation and before any source IR
// is replaced. The plan is borrowed for the duration of the callback.
typedef iree_status_t (*loom_boundary_projection_plan_sink_fn_t)(
    void* user_data, const loom_boundary_projection_plan_t* plan);

typedef struct loom_boundary_projection_plan_sink_t {
  // Consumes one finalized plan.
  loom_boundary_projection_plan_sink_fn_t fn;
  // Opaque caller state passed to |fn|.
  void* user_data;
} loom_boundary_projection_plan_sink_t;

// Plans and atomically applies one composed boundary projection rule set.
iree_status_t loom_boundary_projection_run(
    loom_pass_t* pass, loom_module_t* module,
    const loom_function_version_list_t* version_list,
    loom_boundary_projection_rule_list_t rules,
    const loom_boundary_projection_plan_sink_t* plan_sink,
    loom_boundary_projection_statistics_t* out_statistics);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_BOUNDARY_PROJECTION_DRIVER_H_
