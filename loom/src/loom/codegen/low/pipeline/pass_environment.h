// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-low pass environment capability.
//
// This capability exposes the target-low tables linked into the current
// compiler session without selecting a target for the whole pipeline. Passes
// use the registry and policy tables here, then resolve each function's
// concrete target record from IR facts.

#ifndef LOOM_CODEGEN_LOW_PIPELINE_PASS_ENVIRONMENT_H_
#define LOOM_CODEGEN_LOW_PIPELINE_PASS_ENVIRONMENT_H_

#include "iree/base/api.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/pass/environment.h"
#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Capability type for loom_low_pass_capability_t.
extern const loom_pass_environment_capability_type_t
    loom_low_pass_capability_type;

typedef struct loom_low_lower_policy_registry_t
    loom_low_lower_policy_registry_t;
typedef struct loom_target_low_legality_provider_list_t
    loom_target_low_legality_provider_list_t;
typedef struct loom_target_legalizer_registry_t
    loom_target_legalizer_registry_t;
typedef struct loom_target_compile_report_t loom_target_compile_report_t;

typedef struct loom_low_pass_capability_t {
  // Base capability header. Must remain the first field.
  loom_pass_environment_capability_t base;
  // Linked target-low descriptor registry.
  const loom_low_descriptor_registry_t* descriptor_registry;
  // Linked source-to-target-low lowering policy registry.
  const loom_low_lower_policy_registry_t* lower_policy_registry;
  // Optional target-specific source legality providers linked into this
  // compiler.
  const loom_target_low_legality_provider_list_t* legality_provider_list;
  // Dense source legalizer registry prepared for this compiler environment.
  const loom_target_legalizer_registry_t* legalizer_registry;
  // Optional caller-owned compile report receiving pass-level target feedback.
  loom_target_compile_report_t* compile_report;
} loom_low_pass_capability_t;

// Creates a borrowed low pass capability.
loom_low_pass_capability_t loom_low_pass_capability_make(
    const loom_low_descriptor_registry_t* descriptor_registry,
    const loom_low_lower_policy_registry_t* lower_policy_registry,
    const loom_target_low_legality_provider_list_t* legality_provider_list,
    const loom_target_legalizer_registry_t* legalizer_registry,
    loom_target_compile_report_t* compile_report);

// Looks up the low capability from |environment|. Returns NULL when absent.
const loom_low_pass_capability_t* loom_low_pass_capability_from_environment(
    const loom_pass_environment_t* environment);

// Looks up the low capability from |pass->environment|. Returns NULL when
// absent.
const loom_low_pass_capability_t* loom_low_pass_capability_from_pass(
    const loom_pass_t* pass);

// Returns the descriptor registry selected by |capability|, or NULL.
const loom_low_descriptor_registry_t*
loom_low_pass_capability_descriptor_registry(
    const loom_low_pass_capability_t* capability);

// Returns the lowering policy registry selected by |capability|, or NULL.
const loom_low_lower_policy_registry_t*
loom_low_pass_capability_lower_policy_registry(
    const loom_low_pass_capability_t* capability);

// Returns the legality provider list selected by |capability|, or NULL.
const loom_target_low_legality_provider_list_t*
loom_low_pass_capability_legality_provider_list(
    const loom_low_pass_capability_t* capability);

// Returns the legalizer registry selected by |capability|, or NULL.
const loom_target_legalizer_registry_t*
loom_low_pass_capability_legalizer_registry(
    const loom_low_pass_capability_t* capability);

// Returns the optional compile report selected by |capability|, or NULL.
loom_target_compile_report_t* loom_low_pass_capability_compile_report(
    const loom_low_pass_capability_t* capability);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_PIPELINE_PASS_ENVIRONMENT_H_
