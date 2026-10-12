// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Source-to-Low callable boundary lowering.
//
// A source callable boundary is mapped once and then consumed throughout
// lowering. Validation establishes argument mappings before source planning.
// Source-plan discovery joins the target-neutral exit mappings, and targets
// with physical representation planning refine those joins after their plan is
// solved so exit values and the callable signature consume one retained
// decision.
// Boundary planning retains the final signature and ABI layout before any Low
// operation is created. Definition creation materializes that boundary, entry
// binding connects direct arguments, resource emission materializes arguments
// omitted from the direct ABI, and predicate remapping translates source value
// references after those bindings exist.
//
// Function declarations use the same type and metadata mapping without a body.
// Their retained boundaries let the module owner validate declarations before
// publishing any replacement symbol.

#ifndef LOOM_CODEGEN_LOW_LOWER_FUNCTION_BOUNDARY_H_
#define LOOM_CODEGEN_LOW_LOWER_FUNCTION_BOUNDARY_H_

#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_function_boundary_t {
  // Source argument ABI mappings, in source argument order. Declarations have
  // no resource imports and retain only their direct signature below.
  loom_low_lower_abi_argument_t* argument_map;
  // Number of entries in argument_map.
  uint16_t argument_map_count;
  // Number of direct arguments in the retained Low signature.
  uint16_t argument_count;
  // Direct argument types retained in the function arena.
  loom_type_t* argument_types;
  // Result carriers joined during source planning, in source result order.
  // None until an exit is observed or result mapping is finalized.
  loom_type_t* result_types;
  // Canonical module-owned ABI layout selected from the final signature.
  loom_named_attr_slice_t abi_layout;
} loom_low_lower_function_boundary_t;

// Queries the native ABI representation of a source function argument without
// emitting diagnostics or recording a required boundary mapping. An unsupported
// argument has abi_type none; other output fields are then unused. The default
// direct ABI uses the native value query when the policy has no argument
// mapper. A target may support a native value without supporting it as an ABI
// argument. Allocation failures while constructing a native type propagate
// normally.
iree_status_t loom_low_lower_query_argument(
    loom_low_lower_context_t* context, uint16_t source_argument_index,
    loom_value_id_t source_argument_id,
    loom_low_lower_abi_argument_t* out_argument);

// Validates the source callable boundary, maps arguments, and allocates empty
// result mappings. This must run before source-plan construction.
iree_status_t loom_low_lower_function_boundary_validate(
    loom_low_lower_context_t* context);

// Joins one callable-body exit's native value carriers into the retained
// callable result types. Source-plan discovery calls this for every exit; a
// retained physical representation plan may join the finalized carriers again
// during selection.
iree_status_t loom_low_lower_function_boundary_observe_exit(
    loom_low_lower_context_t* context, const loom_op_t* exit_op);

// Completes result mappings after all exits have been observed. A callable
// without exiting paths retains the target mapping of its declared types.
iree_status_t loom_low_lower_function_boundary_finalize(
    loom_low_lower_context_t* context);

// Retains the definition's final direct signature and ABI layout. Result
// carriers must have been finalized by source planning. Target layout checks
// report authored-input rejection here; temporary callback storage belongs to
// scratch_arena and may be released before definition creation.
iree_status_t loom_low_lower_function_boundary_plan(
    loom_low_lower_context_t* context, iree_arena_allocator_t* scratch_arena);

// Creates the target-Low function or kernel definition for the mapped source
// callable. The definition is inserted immediately before the source op and
// recorded in the lowering context and result.
iree_status_t loom_low_lower_function_boundary_create(
    loom_low_lower_context_t* context, loom_region_t* source_body,
    loom_symbol_ref_t low_func_ref);

// Binds source entry arguments represented directly in the target ABI to the
// corresponding target-Low entry block arguments.
iree_status_t loom_low_lower_function_boundary_bind_entry_arguments(
    loom_low_lower_context_t* context, const loom_block_t* source_entry_block,
    loom_block_t* low_entry_block);

// Remaps source callable predicates across the retained argument and result
// boundary and attaches them to the target-Low definition.
iree_status_t loom_low_lower_function_boundary_remap_predicates(
    loom_low_lower_context_t* context);

// Emits target-Low resource imports for source arguments excluded from the
// direct callable signature and binds their source values.
iree_status_t loom_low_lower_function_boundary_emit_resource_imports(
    loom_low_lower_context_t* context);

typedef struct loom_low_lower_declaration_plan_t
    loom_low_lower_declaration_plan_t;

// Plans one target-bound declaration's native signature and ABI layout without
// changing its source operation or symbol. The plan belongs to |arena| and
// borrows the immutable source declaration. Construction analyses are released
// before returning. Diagnostics populate |out_result| and leave |out_plan|
// NULL.
iree_status_t loom_low_lower_plan_declaration(
    loom_module_t* module, loom_func_like_t source_declaration,
    const loom_low_lower_options_t* options, iree_arena_allocator_t* arena,
    loom_low_lower_result_t* out_result,
    const loom_low_lower_declaration_plan_t** out_plan);

// Publishes a planned low.func.decl without consulting target policy or facts.
//
// The emitted low declaration preserves source symbol identity and callable
// metadata. Runtime imports record the policy import kind and resolved code
// symbol. Ordinary declarations remain unresolved Loom symbols for a subsequent
// IR link. Only allocation failure can interrupt execution of the plan.
iree_status_t loom_low_lower_emit_declaration(
    loom_module_t* module, const loom_low_lower_declaration_plan_t* plan,
    loom_low_lower_result_t* out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_FUNCTION_BOUNDARY_H_
