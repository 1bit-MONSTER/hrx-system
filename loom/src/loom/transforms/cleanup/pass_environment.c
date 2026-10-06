// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/cleanup/pass_environment.h"

#include "loom/transforms/cleanup/pass_requirements.h"

static bool loom_cleanup_pass_capability_satisfies_requirement(
    const loom_pass_environment_capability_t* capability,
    iree_string_view_t requirement) {
  const loom_cleanup_pass_capability_t* cleanup_capability =
      (const loom_cleanup_pass_capability_t*)capability;
  if (iree_string_view_equal(
          requirement,
          IREE_SV(LOOM_CLEANUP_PASS_REQUIREMENT_SOURCE_COMBINE_PATTERNS))) {
    return cleanup_capability->pattern_registry != NULL &&
           cleanup_capability->pattern_registry->source_combine != NULL;
  }
  return false;
}

const loom_pass_environment_capability_type_t
    loom_cleanup_pass_capability_type = {
        .name = IREE_SVL("cleanup"),
        .satisfies_requirement =
            loom_cleanup_pass_capability_satisfies_requirement,
};

loom_cleanup_pass_capability_t loom_cleanup_pass_capability_make(
    const loom_cleanup_pattern_registry_t* pattern_registry,
    loom_cleanup_canonicalizer_context_resolver_t context_resolver) {
  return (loom_cleanup_pass_capability_t){
      .base =
          {
              .type = &loom_cleanup_pass_capability_type,
          },
      .pattern_registry = pattern_registry,
      .context_resolver = context_resolver,
  };
}

const loom_cleanup_pass_capability_t*
loom_cleanup_pass_capability_from_environment(
    const loom_pass_environment_t* environment) {
  if (environment == NULL) {
    return NULL;
  }
  return (const loom_cleanup_pass_capability_t*)loom_pass_environment_lookup(
      environment, &loom_cleanup_pass_capability_type);
}

const loom_cleanup_pass_capability_t* loom_cleanup_pass_capability_from_pass(
    const loom_pass_t* pass) {
  return pass && pass->environment
             ? loom_cleanup_pass_capability_from_environment(pass->environment)
             : NULL;
}

const loom_cleanup_pattern_registry_t*
loom_cleanup_pass_capability_pattern_registry(
    const loom_cleanup_pass_capability_t* capability) {
  return capability ? capability->pattern_registry : NULL;
}

iree_status_t loom_cleanup_pass_capability_resolve_canonicalizer_context(
    const loom_cleanup_pass_capability_t* capability, const loom_pass_t* pass,
    const loom_module_t* module, loom_func_like_t function,
    loom_cleanup_canonicalizer_context_t* out_context) {
  *out_context = (loom_cleanup_canonicalizer_context_t){0};
  if (capability == NULL || capability->context_resolver.fn == NULL) {
    return iree_ok_status();
  }
  return capability->context_resolver.fn(capability->context_resolver.user_data,
                                         pass, module, function, out_context);
}
