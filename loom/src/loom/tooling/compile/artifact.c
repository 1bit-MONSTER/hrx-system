// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/artifact.h"

#include "loom/target/selection.h"

iree_status_t loom_artifact_target_select(
    const loom_artifact_provider_t* provider,
    const loom_target_environment_t* target_environment,
    iree_string_view_t target_specification,
    loom_artifact_target_t* out_target) {
  IREE_ASSERT_ARGUMENT(provider);
  IREE_ASSERT_ARGUMENT(target_environment);
  IREE_ASSERT_ARGUMENT(out_target);
  *out_target = (loom_artifact_target_t){0};

  if (provider->target_profile_type == NULL) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "artifact provider '%.*s' has no target profile type",
        (int)provider->name.size, provider->name.data);
  }

  loom_target_specification_t specification = {0};
  IREE_RETURN_IF_ERROR(
      loom_target_specification_parse(target_specification, &specification));
  const loom_target_profile_t* target_profile = NULL;
  IREE_RETURN_IF_ERROR(loom_target_environment_select_profile(
      target_environment, &specification, &target_profile));
  if (target_profile->type != provider->target_profile_type) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "artifact provider '%.*s' requires target family '%.*s'; got '%.*s'",
        (int)provider->name.size, provider->name.data,
        (int)provider->target_profile_type->name.size,
        provider->target_profile_type->name.data,
        (int)specification.family.size, specification.family.data);
  }

  *out_target = (loom_artifact_target_t){
      .target_profile = target_profile,
      .target_key = specification.selector,
  };
  return iree_ok_status();
}
