// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Loop-carried vector-bank projection for the shared boundary engine.

#ifndef LOOM_TRANSFORMS_VECTOR_BANK_SROA_PROJECTION_H_
#define LOOM_TRANSFORMS_VECTOR_BANK_SROA_PROJECTION_H_

#include "loom/transforms/boundary/projection_rule.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the compiler-owned rule that replaces statically addressed
// loop-carried vector banks with scalar or tail-vector recurrence components.
const loom_boundary_projection_rule_t*
loom_vector_bank_sroa_boundary_projection_rule(void);

typedef struct loom_target_compile_report_t loom_target_compile_report_t;

// Records the finalized source decisions retained by the vector-bank rule.
// The generic projection engine has completed connected rejection propagation
// when this is called, and the source IR has not yet been mutated.
iree_status_t loom_vector_bank_sroa_record_projection_plan(
    const loom_boundary_projection_plan_t* plan,
    loom_target_compile_report_t* report);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_BANK_SROA_PROJECTION_H_
