// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared fact-dependent execution-safety conditions.

#ifndef LOOM_ANALYSIS_EXECUTION_CONDITION_H_
#define LOOM_ANALYSIS_EXECUTION_CONDITION_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_value_fact_table_t loom_value_fact_table_t;

// Returns whether retained operand facts satisfy |op|'s declared execution
// condition. This does not interpret unconditional SAFE_TO_SPECULATE traits.
// The fact table must be an ordinary scope whose facts remain valid when the op
// executes on additional control paths. The query allocates no storage and
// performs no IR traversal.
bool loom_execution_condition_is_satisfied(
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_fact_table_t* fact_table);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_EXECUTION_CONDITION_H_
