// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Source-to-Low normalization for call-like source operations.

#ifndef LOOM_CODEGEN_LOW_LOWER_SOURCE_CALL_H_
#define LOOM_CODEGEN_LOW_LOWER_SOURCE_CALL_H_

#include "iree/base/api.h"
#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns true when |source_op| is a direct semantic CallLike operation owned
// by common source-to-Low lowering. Callable operands and results must occupy
// the complete flat operation boundary.
bool loom_low_lower_source_call_is_structural(const loom_module_t* module,
                                              const loom_op_t* source_op);

// Lowers one direct semantic CallLike operation to low.func.call.
iree_status_t loom_low_lower_source_call(loom_low_lower_context_t* context,
                                         const loom_op_t* source_op);

// Normalizes a source-typed low.invoke into a register-typed low.func.call.
// Target selection has already projected the callee representation into the
// caller's exact Low contract before function lowering begins.
iree_status_t loom_low_lower_source_invoke(loom_low_lower_context_t* context,
                                           const loom_op_t* source_op);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_SOURCE_CALL_H_
