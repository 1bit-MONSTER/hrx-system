// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Prepared HAL execution profile for check.scenario kernel and function
// subjects.

#ifndef LOOM_TOOLING_EXECUTION_HAL_SCENARIO_PROFILE_H_
#define LOOM_TOOLING_EXECUTION_HAL_SCENARIO_PROFILE_H_

#include "loom/tooling/execution/hal/testbench_actual.h"
#include "loom/tooling/testbench/scenario/executor.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compiler and runtime inputs shared by every product prepared through one
// externally selected HAL scenario profile.
typedef struct loom_run_hal_testbench_scenario_profile_t {
  // Stable profile name surfaced in scenario results and diagnostics.
  iree_string_view_t name;
  // Provider options copied into each independently prepared HAL product. The
  // run module and kernel launch may be replaced by a target-only function
  // adapter during product preparation.
  loom_run_hal_testbench_actual_provider_options_t provider_options;
} loom_run_hal_testbench_scenario_profile_t;

// Initializes a borrowing HAL scenario profile without compiling a product.
void loom_run_hal_testbench_scenario_profile_initialize(
    iree_string_view_t name,
    const loom_run_hal_testbench_actual_provider_options_t* provider_options,
    loom_run_hal_testbench_scenario_profile_t* out_profile);

// Returns the external execution profile backed by |profile|.
//
// Product preparation compiles and loads one ordinary kernel artifact before
// any trial-local values exist. An ordinary void function with scalar and
// buffer arguments is privately adapted to a fixed 1x1 kernel in the target
// product; the authored module remains unchanged for oracle execution. Product
// execution records every call in a bounded batch into one command sequence,
// stages each distinct host allocation once while preserving aliases, submits
// once, and reads completed state back. Product benchmarking reuses that same
// command sequence after target-local staging and omits readback.
loom_testbench_execution_profile_t
loom_run_hal_testbench_scenario_execution_profile(
    loom_run_hal_testbench_scenario_profile_t* profile);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_EXECUTION_HAL_SCENARIO_PROFILE_H_
