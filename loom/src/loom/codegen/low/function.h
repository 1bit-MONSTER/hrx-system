// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-low function contracts and body materialization.
//
// Low helper functions and low kernel entries share the same structural body
// model and descriptor/scheduler/allocation machinery. These helpers keep that
// production contract centralized so adding a specialized entry op does not
// fork the backend pipeline. Body materialization preserves function-level
// contracts when inlining or target construction removes a function boundary.

#ifndef LOOM_CODEGEN_LOW_FUNCTION_H_
#define LOOM_CODEGEN_LOW_FUNCTION_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_builder_t loom_builder_t;

// Returns true for low function definitions with executable bodies.
bool loom_low_function_def_isa(const loom_op_t* op);

// Returns the module-local symbol ref naming |function_op|.
loom_symbol_ref_t loom_low_function_callee(const loom_op_t* function_op);

// Returns the module-local target record symbol ref selected by |function_op|.
loom_symbol_ref_t loom_low_function_target(const loom_op_t* function_op);

// Returns the low allocation mode attr value, or 0 when absent.
uint8_t loom_low_function_allocation(const loom_op_t* function_op);

// Returns the low schedule mode attr value, or 0 when absent.
uint8_t loom_low_function_schedule(const loom_op_t* function_op);

// Returns the executable body region of |function_op|, or NULL when absent.
loom_region_t* loom_low_function_body(loom_op_t* function_op);

// Returns the executable body region of |function_op|, or NULL when absent.
const loom_region_t* loom_low_function_const_body(const loom_op_t* function_op);

// Materializes a verified Low body's implicit function scheduling contract as
// explicit controls in |blocks|. |schedule| uses the mode returned by
// loom_low_function_schedule. The span begins with the source entry block and
// includes every source return, before the caller splices or rewrites them.
// It may refer to a whole function body or its freshly cloned blocks inside a
// larger region. Only those blocks are changed; existing explicit controls and
// the entry live-in/resource preamble remain in place.
//
// Locked bodies receive per-instruction fences. Phased bodies receive a scope
// beginning after the entry preamble and ending before every source return.
// Other modes require no translation or traversal. These controls impose
// compiler order, not runtime synchronization or hardware completion.
//
// The caller owns the mutable blocks and materializes the contract once. A
// retained source function must have its translated schedule mode cleared;
// a cloned body leaves its immutable source untouched. The builder's insertion
// point changes. Failure propagates only from allocating the new control ops.
iree_status_t loom_low_function_materialize_schedule(
    loom_builder_t* builder, uint8_t schedule, loom_block_t* const* blocks,
    uint16_t block_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_FUNCTION_H_
