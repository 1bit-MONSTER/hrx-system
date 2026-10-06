// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Low function representation and target binding.
//
// This layer resolves a Low function's intrinsic representation contract to
// dense Low descriptor tables, then verifies that the representation can
// encode the target selected by authored or invocation-refined target facts.
// The descriptor table ABI itself remains IR-agnostic.

#ifndef LOOM_CODEGEN_LOW_TARGET_BINDING_H_
#define LOOM_CODEGEN_LOW_TARGET_BINDING_H_

#include "iree/base/api.h"
#include "loom/analysis/symbol_facts.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/error/emitter.h"
#include "loom/ir/ir.h"
#include "loom/ops/low/ops.h"
#include "loom/target/facts.h"
#include "loom/target/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resolved low target context for one low function.
typedef struct loom_low_resolved_target_t {
  // Immutable function target facts selected for this Low function, or NULL
  // for a portable representation that does not require a hardware target.
  const loom_target_facts_t* target_facts;
  // Borrowed effective target name without the leading '@', or empty when
  // |target_facts| is NULL.
  iree_string_view_t target_name;
  // Borrowed descriptor-set key selected by the Low function representation
  // contract.
  iree_string_view_t descriptor_set_key;
  // Feature bitset projected from the function target facts.
  uint64_t feature_bits;
  // Descriptor set found in the caller-provided registry.
  const loom_low_descriptor_set_t* descriptor_set;
} loom_low_resolved_target_t;

// Returns the common target bundle projected into |target->target_facts|.
static inline const loom_target_bundle_t* loom_low_resolved_target_bundle(
    const loom_low_resolved_target_t* target) {
  return target ? loom_target_facts_bundle(target->target_facts) : NULL;
}

// Resolves the function target facts and descriptor set for |low_func_op|
// using caller-owned symbol facts.
// Supplied |function_target_facts| already include the function contract and
// are consumed without symbol-fact queries. Kernel workgroup-size refinement
// allocates its result from the symbol-fact table's arena without populating
// the table.
//
// User IR failures are emitted through |emitter| and leave
// out_target->descriptor_set NULL. Infrastructure failures are returned as
// status. |low_func_op| must be a target-low function definition or
// declaration. |function_target_facts| supplies invocation-refined facts that
// already include the function contract when non-NULL; otherwise facts are
// resolved from the authored target witness. A targetless function resolves
// only its explicit representation descriptor set and leaves the target facts
// unset, independent of its ABI. The arena backing |symbol_facts| and
// |function_target_facts| must outlive |out_target|.
iree_status_t loom_low_resolve_function_target(
    const loom_module_t* module, loom_symbol_fact_table_t* symbol_facts,
    const loom_op_t* low_func_op,
    const loom_target_facts_t* function_target_facts,
    const loom_low_descriptor_registry_t* registry,
    iree_diagnostic_emitter_t emitter, loom_low_resolved_target_t* out_target);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_TARGET_BINDING_H_
