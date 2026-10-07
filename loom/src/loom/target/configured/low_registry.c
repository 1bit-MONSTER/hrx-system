// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/configured/low_registry.h"

#include "loom/target/configured/provider.h"

void loom_configured_low_descriptor_registry_initialize(
    loom_target_low_descriptor_registry_t* out_registry) {
  *out_registry = loom_target_environment_low_descriptor_registry(
      loom_configured_target_environment());
}

void loom_configured_low_lower_policy_registry_initialize(
    loom_low_lower_policy_registry_t* out_registry) {
  *out_registry = loom_target_environment_low_lower_policy_registry(
      loom_configured_target_environment());
}

loom_target_low_legality_provider_list_t
loom_configured_low_legality_provider_list(void) {
  return loom_target_environment_low_legality_provider_list(
      loom_configured_target_environment());
}

loom_target_legalizer_provider_list_t loom_configured_legalizer_provider_list(
    void) {
  return loom_target_environment_legalizer_provider_list(
      loom_configured_target_environment());
}
