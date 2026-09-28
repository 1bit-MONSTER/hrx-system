// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_CODEGEN_LOW_LOWER_SOURCE_MEMORY_H_
#define LOOM_CODEGEN_LOW_LOWER_SOURCE_MEMORY_H_

#include "loom/codegen/low/source_memory_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_context_t loom_low_lower_context_t;
typedef struct loom_low_lower_source_memory_builder_t
    loom_low_lower_source_memory_builder_t;

// Canonical source access retained by the shared preselection traversal.
typedef struct loom_low_lower_source_memory_record_t {
  // Operation whose logical memory origin is described by access.
  const loom_op_t* source_op;
  // Next access in shared traversal order, or NULL at the end.
  struct loom_low_lower_source_memory_record_t* next;
  // Function-owned canonical access, finalized before per-operation selection.
  loom_low_source_memory_access_plan_t access;
  // Source-level rejection preserved for target selection diagnostics.
  loom_low_source_memory_access_diagnostic_t diagnostic;
  // True when canonical source planning accepted this access.
  bool available;
} loom_low_lower_source_memory_record_t;

// Creates component interning scratch in the planning arena. The caller feeds
// the existing dominance-ordered source traversal; no second IR walk is made.
iree_status_t loom_low_lower_source_memory_builder_create(
    loom_low_lower_context_t* context,
    loom_low_lower_source_memory_builder_t** out_builder);

// Retains memory accesses and publishes the current record before target
// observation. Fragment accesses describe logical origins, not payload spans.
iree_status_t loom_low_lower_source_memory_observe(
    loom_low_lower_source_memory_builder_t* builder,
    loom_low_lower_context_t* context, const loom_op_t* source_op);

// Advances the retained access cursor alongside per-operation selection.
void loom_low_lower_source_memory_select_op(loom_low_lower_context_t* context,
                                            const loom_op_t* source_op);

// Returns the current operation's canonical plan, or NULL with its retained
// diagnostic when source planning rejected it. Observation and selection both
// expose the current record; consumers do not rebuild canonical addresses.
const loom_low_source_memory_access_plan_t* loom_low_lower_source_memory_access(
    const loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_source_memory_access_diagnostic_t* out_diagnostic);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_SOURCE_MEMORY_H_
