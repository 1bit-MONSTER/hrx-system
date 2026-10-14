// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Physical input/result ties for independently owned Low results.

#ifndef LOOM_CODEGEN_LOW_OPERAND_TIES_H_
#define LOOM_CODEGEN_LOW_OPERAND_TIES_H_

#include "iree/base/api.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/codegen/low/memory_access.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_rewriter_t loom_rewriter_t;

// Returns whether a verified packet has a required physical input/result tie
// without the corresponding IR ownership tie. Other operations return false.
bool loom_low_operand_ties_need_materialization(
    const loom_low_descriptor_set_t* descriptor_set, const loom_op_t* op);

// Gives each independent, physically tied result its own copied input. Existing
// ownership ties remain intact, and allocation may coalesce the new copies when
// source lifetimes permit. Replaces |op| through |rewriter| while preserving
// its attributes, source location, result names, uses and optional memory
// summaries. Returns |op| unchanged when no copies are required. Scratch
// allocations belong to the rewriter's arena; replacement IR belongs to its
// module. Callers run this during Low preparation, before scheduling or
// retaining allocation facts.
iree_status_t loom_low_operand_ties_materialize(
    loom_rewriter_t* rewriter, const loom_low_descriptor_set_t* descriptor_set,
    loom_low_memory_access_map_t* memory_accesses, loom_op_t* op,
    loom_op_t** out_replacement);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_OPERAND_TIES_H_
