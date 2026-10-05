// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/boundary/projection.h"

#include "loom/target/pass_environment.h"
#include "loom/transforms/boundary/loop_predicate_projection.h"
#include "loom/transforms/boundary/projection_driver.h"
#include "loom/transforms/encoding/layout_projection.h"
#include "loom/transforms/view/boundary_projection.h"

#define LOOM_BOUNDARY_PROJECTION_STATISTICS(V, statistics_type)                \
  V(statistics_type, functions_rewritten, "functions-rewritten",               \
    "Number of function signatures rewritten.")                                \
  V(statistics_type, calls_rewritten, "calls-rewritten",                       \
    "Number of semantic calls rewritten.")                                     \
  V(statistics_type, returns_rewritten, "returns-rewritten",                   \
    "Number of returns rewritten.")                                            \
  V(statistics_type, cfg_edges_rewritten, "cfg-edges-rewritten",               \
    "Number of CFG successor payloads rewritten.")                             \
  V(statistics_type, loops_rewritten, "loops-rewritten",                       \
    "Number of LoopLike operations rewritten.")                                \
  V(statistics_type, loop_predicate_projections, "loop-predicate-projections", \
    "Number of lane predicates projected to durable loop words.")              \
  V(statistics_type, loop_predicate_components, "loop-predicate-components",   \
    "Number of physical predicate carrier components materialized.")           \
  V(statistics_type, layout_projections, "layout-projections",                 \
    "Number of dynamic layouts projected to scalar components.")               \
  V(statistics_type, layout_components, "layout-components",                   \
    "Number of scalar layout components materialized.")                        \
  V(statistics_type, view_projections, "view-projections",                     \
    "Number of retained views projected to physical carriers.")                \
  V(statistics_type, view_components, "view-components",                       \
    "Number of physical view carrier components materialized.")

LOOM_PASS_STATISTICS_DEFINE(loom_boundary_projection_statistics,
                            loom_boundary_projection_pass_statistics_t,
                            LOOM_BOUNDARY_PROJECTION_STATISTICS)

static const loom_pass_info_t kPassInfo = {
    .name = IREE_SVL("project-boundary-representations"),
    .description = IREE_SVL(
        "Compose physical representation projections across boundaries."),
    .kind = LOOM_PASS_MODULE,
    .statistic_layout = &loom_boundary_projection_statistics_layout,
};

static const loom_pass_info_t kLoopPassInfo = {
    .name = IREE_SVL("project-loop-boundary-representations"),
    .description =
        IREE_SVL("Project physical representations across structured loops."),
    .kind = LOOM_PASS_MODULE,
    .statistic_layout = &loom_boundary_projection_statistics_layout,
};

const loom_pass_info_t* loom_project_boundary_representations_pass_info(void) {
  return &kPassInfo;
}

const loom_pass_info_t* loom_project_loop_boundary_representations_pass_info(
    void) {
  return &kLoopPassInfo;
}

static iree_status_t loom_project_boundary_representations_run_rules(
    loom_pass_t* pass, loom_module_t* module,
    loom_boundary_projection_rule_list_t rules) {
  const loom_target_pass_capability_t* capability =
      loom_target_pass_capability_from_pass(pass);
  IREE_ASSERT(capability != NULL);
  const loom_function_version_list_t* version_list =
      loom_target_pass_capability_function_versions(capability);
  IREE_ASSERT(version_list != NULL);
  IREE_ASSERT(loom_target_pass_capability_function_version_owner(capability));

  loom_boundary_projection_statistics_t projection_statistics;
  iree_status_t status =
      loom_boundary_projection_run(pass, module, version_list, rules,
                                   /*plan_sink=*/NULL, &projection_statistics);

  if (iree_status_is_ok(status)) {
    IREE_ASSERT_EQ(projection_statistics.rule_count, rules.count);
    loom_boundary_projection_pass_statistics_t* statistics =
        loom_boundary_projection_statistics(pass);
    statistics->functions_rewritten +=
        projection_statistics.functions_rewritten;
    statistics->calls_rewritten += projection_statistics.calls_rewritten;
    statistics->returns_rewritten += projection_statistics.returns_rewritten;
    statistics->cfg_edges_rewritten +=
        projection_statistics.cfg_edges_rewritten;
    statistics->loops_rewritten += projection_statistics.loops_rewritten;
    for (iree_host_size_t i = 0; i < rules.count; ++i) {
      const loom_boundary_projection_rule_t* rule = rules.values[i];
      const loom_boundary_projection_rule_statistics_t rule_statistics =
          projection_statistics.rules[i];
      if (rule == loom_loop_predicate_boundary_projection_rule()) {
        statistics->loop_predicate_projections += rule_statistics.projections;
        statistics->loop_predicate_components += rule_statistics.components;
      } else if (rule == loom_cfg_layout_boundary_projection_rule() ||
                 rule == loom_loop_layout_boundary_projection_rule()) {
        statistics->layout_projections += rule_statistics.projections;
        statistics->layout_components += rule_statistics.components;
      } else if (rule == loom_view_boundary_projection_rule()) {
        statistics->view_projections += rule_statistics.projections;
        statistics->view_components += rule_statistics.components;
      }
    }
  }
  return status;
}

iree_status_t loom_project_boundary_representations_run(loom_pass_t* pass,
                                                        loom_module_t* module) {
  const loom_boundary_projection_rule_t* rules[] = {
      loom_loop_predicate_boundary_projection_rule(),
      loom_cfg_layout_boundary_projection_rule(),
      loom_loop_layout_boundary_projection_rule(),
      loom_view_boundary_projection_rule(),
  };
  return loom_project_boundary_representations_run_rules(
      pass, module,
      (loom_boundary_projection_rule_list_t){
          .values = rules,
          .count = IREE_ARRAYSIZE(rules),
      });
}

iree_status_t loom_project_loop_boundary_representations_run(
    loom_pass_t* pass, loom_module_t* module) {
  const loom_boundary_projection_rule_t* rules[] = {
      loom_loop_predicate_boundary_projection_rule(),
      loom_loop_layout_boundary_projection_rule(),
  };
  return loom_project_boundary_representations_run_rules(
      pass, module,
      (loom_boundary_projection_rule_list_t){
          .values = rules,
          .count = IREE_ARRAYSIZE(rules),
      });
}
