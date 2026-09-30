// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Compiler product, root, target, and format request resolution.

#ifndef LOOM_TOOLING_COMPILE_REQUEST_H_
#define LOOM_TOOLING_COMPILE_REQUEST_H_

#include "iree/base/api.h"
#include "loom/compile/product_selection.h"
#include "loom/target/selection.h"

#ifdef __cplusplus
extern "C" {
#endif

// Concrete producer selected for one compile request.
typedef enum loom_compile_producer_kind_e {
  LOOM_COMPILE_PRODUCER_INVALID = 0,
  LOOM_COMPILE_PRODUCER_COMMAND = 1,
  LOOM_COMPILE_PRODUCER_TARGET_EMITTER = 2,
} loom_compile_producer_kind_t;

typedef struct loom_compile_producer_t {
  // Selected producer kind.
  loom_compile_producer_kind_t kind;
  // Target-owned module or kernel artifact emitter, or NULL for commands.
  const loom_target_emitter_t* target_emitter;
} loom_compile_producer_t;

// Explicit target selected for one compile request.
typedef struct loom_compile_target_selection_t {
  // Immutable structured target profile selected from the environment.
  const loom_target_profile_t* profile;
  // Borrowed family and selector spelling supplied by the caller.
  loom_target_specification_t specification;
} loom_compile_target_selection_t;

// User constraints applied while resolving one compilation request.
typedef struct loom_compile_request_options_t {
  // Explicit root names, or an empty list to derive selection from the module.
  iree_string_view_list_t roots;
  // Optional product selection or explicit-root assertion.
  iree_string_view_t product;
  // Optional exact artifact format.
  iree_string_view_t format;
  // Optional family-qualified target profile.
  iree_string_view_t target;
  // Canonical root names to exclude after product inference and before
  // specialization and materialization. Cannot be combined with |roots|.
  iree_string_view_list_t excluded_roots;
} loom_compile_request_options_t;

// Fully resolved compile request borrowing immutable configured state.
typedef struct loom_compile_request_t {
  // Language-level product and root selection.
  loom_compile_product_selection_t selection;
  // Exact public artifact format.
  iree_string_view_t format;
  // Producer implementing |format| for |product|.
  loom_compile_producer_t producer;
  // Explicit target selected by --target, or empty for authored targets.
  loom_compile_target_selection_t explicit_target;
  // Effective target fact type after explicit target selection, or NULL for
  // target-independent products.
  const loom_target_fact_type_t* target_fact_type;
} loom_compile_request_t;

// Returns true when portable command emission was selected.
static inline bool loom_compile_request_is_command(
    const loom_compile_request_t* request) {
  return request != NULL &&
         request->producer.kind == LOOM_COMPILE_PRODUCER_COMMAND;
}

// Resolves a core product selection, optional explicit target, and producer.
// Resolution never probes a producer by compiling. An omitted format selects
// the selected target family's unique canonical kernel or module producer, or
// the target-independent command format.
iree_status_t loom_compile_request_resolve(
    const loom_module_t* module, const loom_compile_request_options_t* options,
    const loom_target_environment_t* target_environment,
    iree_arena_allocator_t* arena, loom_compile_request_t* out_request);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_COMPILE_REQUEST_H_
