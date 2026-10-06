// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Per-function target specialization.
//
// Specialization is an invocation-local compiler action. Each request names
// one function version and supplies the structured target profile that should
// become its exact resolved target. It produces compiler-owned function
// versions without creating target records or changing authored target attrs.

#ifndef LOOM_TARGET_SPECIALIZATION_H_
#define LOOM_TARGET_SPECIALIZATION_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/error/emitter.h"
#include "loom/ir/ir.h"
#include "loom/target/function_version.h"
#include "loom/target/profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_target_environment_t loom_target_environment_t;

// One function version to specialize to one structured target profile.
typedef struct loom_target_specialization_request_t {
  // Function symbol name, with an optional leading '@'.
  iree_string_view_t function_name;

  // Structured target profile borrowed for the specialization lifetime.
  const loom_target_profile_t* target_profile;
} loom_target_specialization_request_t;

// Borrowed list of per-function target specialization requests.
typedef struct loom_target_specialization_request_list_t {
  // Request rows, or NULL when |count| is zero.
  const loom_target_specialization_request_t* values;

  // Number of request rows in |values|.
  iree_host_size_t count;
} loom_target_specialization_request_list_t;

// One authored target declaration to bind to one structured target profile.
typedef struct loom_target_declaration_binding_t {
  // Target declaration symbol name, with an optional leading '@'.
  iree_string_view_t target_name;

  // Structured target profile borrowed for the specialization lifetime.
  const loom_target_profile_t* target_profile;
} loom_target_declaration_binding_t;

// Borrowed list of target declaration bindings.
typedef struct loom_target_declaration_binding_list_t {
  // Binding rows, or NULL when |count| is zero.
  const loom_target_declaration_binding_t* values;

  // Number of binding rows in |values|.
  iree_host_size_t count;
} loom_target_declaration_binding_list_t;

// Resolves target specialization requests and declaration bindings into
// function versions.
//
// The input module must be verified for this compiler invocation. All function
// names, target declaration names, profiles, authored target requirements, and
// function-local contracts are resolved before any versions are published. A
// declaration binding seeds every target-assignable function authored against
// that declaration. Direct function requests and declaration-derived requests
// must be disjoint. Selected definitions become retained compiler entries
// without changing source visibility, retention flags, or target witnesses.
// Source incompatibilities emit structured diagnostics and return
// OK with a nonzero |error_count|; malformed external requests and
// infrastructure failures return a status.
//
// |inout_function_versions| owns any versions retained by prior compiler
// stages and receives the specialized versions. Re-specializing a live
// function preserves its stable version identity and captured lowering
// products while replacing its target facts. New target contexts receive
// ordinals after every context observed in the existing owner. Its arena must
// outlive every pass and output consumer that uses the versions. Target
// profiles need only remain live for this call.
//
// Source incompatibilities leave |inout_function_versions| unchanged and are
// reported through |out_error_count|.
iree_status_t loom_target_specialize_functions(
    const loom_target_environment_t* environment, loom_module_t* module,
    loom_target_specialization_request_list_t requests,
    loom_target_declaration_binding_list_t bindings,
    iree_diagnostic_emitter_t diagnostic_emitter,
    loom_function_version_owner_t* inout_function_versions,
    uint32_t* out_error_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_SPECIALIZATION_H_
