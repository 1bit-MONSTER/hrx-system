// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Options and statistics contract for target source legalization.

#ifndef LOOM_CODEGEN_LOW_TRANSFORMS_PIPELINE_TARGET_LEGALIZE_OPTIONS_H_
#define LOOM_CODEGEN_LOW_TRANSFORMS_PIPELINE_TARGET_LEGALIZE_OPTIONS_H_

#include "iree/base/api.h"
#include "loom/pass/types.h"
#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_target_legalize_pass_state_t {
  // Maximum number of fixed-point rewrite iterations.
  uint32_t max_iterations;
  // Maximum number of final legality diagnostics emitted before stopping.
  uint32_t max_errors;
  // Current target legalization phase.
  loom_target_legalization_mode_t mode;
  // Representation state of semantic boundaries seen by this invocation.
  loom_target_boundary_representation_t boundary_representation;
  // Strategy policy controlling native/reference legalizer participation.
  loom_target_legalization_policy_t policy;
  // Verify source legality here when this pass is the final consumer. Composed
  // pipelines leave verification to source-to-low after structural lowering.
  bool verify_source_legality;
  // True when max_iterations was explicitly provided.
  bool has_max_iterations_option;
  // True when max_errors was explicitly provided.
  bool has_max_errors_option;
  // True when mode was explicitly provided.
  bool has_mode_option;
  // True when boundaries was explicitly provided.
  bool has_boundaries_option;
  // True when policy was explicitly provided.
  bool has_policy_option;
} loom_low_target_legalize_pass_state_t;

#define LOOM_LOW_TARGET_LEGALIZE_STATISTICS(V, statistics_type)      \
  V(statistics_type, ops_rewritten, "ops-rewritten",                 \
    "Number of source ops rewritten by target legalizers.")          \
  V(statistics_type, ops_legal, "ops-legal",                         \
    "Number of legalizer-rooted ops already accepted by the target " \
    "contract.")                                                     \
  V(statistics_type, ops_deferred, "ops-deferred",                   \
    "Number of legalizer-rooted ops left for a later phase.")        \
  V(statistics_type, functions, "functions",                         \
    "Number of target-bound source functions visited.")              \
  V(statistics_type, errors, "errors",                               \
    "Number of final legality errors emitted.")

LOOM_PASS_STATISTICS_DECLARE(loom_low_target_legalize_statistics,
                             loom_low_target_legalize_statistics_t,
                             LOOM_LOW_TARGET_LEGALIZE_STATISTICS)

extern const loom_pass_statistic_layout_t
    loom_low_target_legalize_statistics_layout;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_TRANSFORMS_PIPELINE_TARGET_LEGALIZE_OPTIONS_H_
