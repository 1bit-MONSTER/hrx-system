// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Prepared SPIR-V program consumed by binary emission.

#ifndef LOOM_TARGET_EMIT_SPIRV_PROGRAM_H_
#define LOOM_TARGET_EMIT_SPIRV_PROGRAM_H_

#include "iree/base/api.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/ir/ir.h"
#include "loom/target/facts.h"
#include "loom/target/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// One verified Low function and its target-specific representation binding.
typedef struct loom_spirv_function_plan_t {
  // Structured Low function definition emitted into the SPIR-V module.
  loom_op_t* function_op;
  // Function-specific target facts carrying its bundle and export plan.
  const loom_target_facts_t* target_facts;
  // Dense descriptor set selected by the function's Low representation.
  const loom_low_descriptor_set_t* descriptor_set;
} loom_spirv_function_plan_t;

// Returns the target bundle retained by |function_plan|.
static inline const loom_target_bundle_t*
loom_spirv_function_plan_target_bundle(
    const loom_spirv_function_plan_t* function_plan) {
  return loom_target_facts_bundle(function_plan->target_facts);
}

// Immutable SPIR-V program produced by compiler preparation.
//
// The function table contains only entries selected for this artifact. All
// entries have already resolved a concrete SPIR-V target, selected the
// spirv.logical.core descriptor set, and agreed on one module contract.
typedef struct loom_spirv_program_plan_t {
  // Module containing the prepared function bodies. Emission preserves
  // semantic IR and uses only module-owned value-domain scratch.
  loom_module_t* module;
  // Functions in compiler-selected emission order.
  const loom_spirv_function_plan_t* functions;
  // Number of entries in |functions|. Prepared programs are never empty.
  iree_host_size_t function_count;
} loom_spirv_program_plan_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_SPIRV_PROGRAM_H_
