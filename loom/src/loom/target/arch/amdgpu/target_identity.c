// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/target_identity.h"

#include "loom/target/arch/amdgpu/target_info.h"

void loom_amdgpu_target_identity_initialize(
    const loom_amdgpu_target_info_t* target,
    loom_amdgpu_target_identity_t* out_identity) {
  IREE_ASSERT_ARGUMENT(target);
  IREE_ASSERT_ARGUMENT(out_identity);
  const loom_amdgpu_processor_info_t* processor =
      loom_amdgpu_target_info_target_processor(target);
  IREE_ASSERT(processor != NULL);
  *out_identity = (loom_amdgpu_target_identity_t){.target = target};
  loom_amdgpu_amdhsa_feature_states_initialize(processor,
                                               &out_identity->amdhsa_features);
}

bool loom_amdgpu_target_identity_equal(
    const loom_amdgpu_target_identity_t* lhs,
    const loom_amdgpu_target_identity_t* rhs) {
  if (lhs == NULL || rhs == NULL || lhs->target == NULL ||
      lhs->target != rhs->target) {
    return false;
  }

  loom_amdgpu_target_id_feature_support_flags_t remaining_features =
      LOOM_AMDGPU_TARGET_ID_FEATURE_SUPPORT_KNOWN_FLAGS;
  while (remaining_features != 0) {
    const loom_amdgpu_target_id_feature_support_bit_t feature =
        (loom_amdgpu_target_id_feature_support_bit_t)(remaining_features &
                                                      (0u -
                                                       remaining_features));
    remaining_features &= ~feature;
    if (loom_amdgpu_amdhsa_feature_state_query(&lhs->amdhsa_features,
                                               feature) !=
        loom_amdgpu_amdhsa_feature_state_query(&rhs->amdhsa_features,
                                               feature)) {
      return false;
    }
  }
  return true;
}

bool loom_amdgpu_target_identity_satisfies_requirement(
    const loom_amdgpu_target_identity_t* effective,
    const loom_amdgpu_target_identity_t* requirement) {
  if (effective == NULL || effective->target == NULL || requirement == NULL ||
      requirement->target == NULL ||
      !loom_amdgpu_target_satisfies_code_object_requirement(
          effective->target, requirement->target)) {
    return false;
  }

  const loom_amdgpu_processor_info_t* requirement_processor =
      loom_amdgpu_target_info_target_processor(requirement->target);
  loom_amdgpu_target_id_feature_support_flags_t remaining_features =
      requirement_processor->target_id.supported_features;
  IREE_ASSERT(iree_all_bits_set(
      LOOM_AMDGPU_TARGET_ID_FEATURE_SUPPORT_KNOWN_FLAGS, remaining_features));
  while (remaining_features != 0) {
    const loom_amdgpu_target_id_feature_support_bit_t feature =
        (loom_amdgpu_target_id_feature_support_bit_t)(remaining_features &
                                                      (0u -
                                                       remaining_features));
    remaining_features &= ~feature;
    const loom_amdgpu_target_feature_state_t required_state =
        loom_amdgpu_amdhsa_feature_state_query(&requirement->amdhsa_features,
                                               feature);
    if (required_state == LOOM_AMDGPU_TARGET_FEATURE_ANY) {
      continue;
    }
    if (loom_amdgpu_amdhsa_feature_state_query(&effective->amdhsa_features,
                                               feature) != required_state) {
      return false;
    }
  }
  return true;
}
