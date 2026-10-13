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
#include "loom/ir/types.h"
#include "loom/pass/environment.h"
#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pass requirement satisfied when the pass environment provides a target-low
// descriptor registry for low-codegen passes.
#define LOOM_LOW_PASS_REQUIREMENT_TARGET_LOW_DESCRIPTOR_REGISTRY \
  "target.low-descriptor-registry"

// Pass requirement satisfied when the pass environment provides a
// source-to-target-low lowering policy registry.
#define LOOM_LOW_PASS_REQUIREMENT_TARGET_LOW_LOWER_POLICY_REGISTRY \
  "target.low-lower-policy-registry"

// Pass requirement satisfied when the pass environment provides a dense
// source legalizer registry.
#define LOOM_LOW_PASS_REQUIREMENT_TARGET_LEGALIZER_REGISTRY \
  "target.legalizer-registry"

// Capability type for loom_low_pass_capability_t.
extern const loom_pass_environment_capability_type_t
    loom_low_pass_capability_type;

typedef struct loom_low_lower_policy_registry_t
    loom_low_lower_policy_registry_t;
typedef struct loom_module_t loom_module_t;
typedef struct loom_target_facts_t loom_target_facts_t;
typedef struct loom_target_vector_packet_policy_t
    loom_target_vector_packet_policy_t;
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

// Selects the vector packet policy for |target_facts|, or NULL when the pass
// environment or selected target has no packet policy.
const loom_target_vector_packet_policy_t*
loom_low_pass_capability_vector_packet_policy(
    const loom_low_pass_capability_t* capability,
    const loom_target_facts_t* target_facts);

// Returns whether |type| has a direct source-operation carrier for the target
// selected by |target_facts|. Missing target or carrier policies reject the
// query; a selected policy with no callback admits the type.
bool loom_low_pass_capability_source_vector_carrier_supported(
    const loom_low_pass_capability_t* capability,
    const loom_target_facts_t* target_facts, const loom_module_t* module,
    loom_type_t type);

// Returns whether |type| has a direct callable carrier for the target selected
// by |target_facts|. Missing callable callbacks use the source-operation
// carrier contract.
bool loom_low_pass_capability_source_function_vector_carrier_supported(
    const loom_low_pass_capability_t* capability,
    const loom_target_facts_t* target_facts, const loom_module_t* module,
    loom_type_t type);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_PIPELINE_PASS_ENVIRONMENT_H_
