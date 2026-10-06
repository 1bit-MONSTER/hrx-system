// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Split kernel-barrier lifetime analysis.
//
// Local op verification owns phase types and the one-arrival/one-wait SSA
// relationship. This analysis proves that the phase is also balanced along
// every structured and CFG control path.

#ifndef LOOM_ANALYSIS_KERNEL_BARRIER_LIFETIME_H_
#define LOOM_ANALYSIS_KERNEL_BARRIER_LIFETIME_H_

#include "iree/base/api.h"
#include "loom/error/emitter.h"
#include "loom/ir/ir.h"
#include "loom/ir/local_value_domain.h"
#include "loom/ir/module.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_kernel_barrier_lifetime_options_t {
  // Function-local value definitions used to skip CFG analysis when the
  // function contains no split-barrier arrival.
  const loom_local_value_domain_t* value_domain;
  // Borrowed function-local facts containing the retained CFG for each region.
  const loom_value_fact_table_t* fact_table;
  // Structured diagnostic emitter for authored lifetime failures.
  iree_diagnostic_emitter_t emitter;
  // Name of the phase reporting diagnostics, such as "source-low".
  iree_string_view_t phase_name;
} loom_kernel_barrier_lifetime_options_t;

typedef struct loom_kernel_barrier_lifetime_result_t {
  // Number of error diagnostics emitted.
  uint32_t error_count;
} loom_kernel_barrier_lifetime_result_t;

// Verifies split-barrier lifetimes for one function-like body.
//
// |options->fact_table| must contain the current function's retained region CFG
// facts. Authored IR failures are emitted and counted while allocation failures
// are returned. The analysis stops after the first lifetime failure because
// subsequent block states are no longer meaningful.
iree_status_t loom_kernel_barrier_lifetime_verify_function(
    const loom_module_t* module, loom_func_like_t function,
    const loom_kernel_barrier_lifetime_options_t* options,
    loom_kernel_barrier_lifetime_result_t* out_result);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_ANALYSIS_KERNEL_BARRIER_LIFETIME_H_
