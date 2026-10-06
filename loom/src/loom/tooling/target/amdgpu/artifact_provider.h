// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Offline AMDGPU HAL-ABI artifact compilation.

#ifndef LOOM_TOOLING_TARGET_AMDGPU_ARTIFACT_PROVIDER_H_
#define LOOM_TOOLING_TARGET_AMDGPU_ARTIFACT_PROVIDER_H_

#include "loom/tooling/execution/hal/artifact.h"

#ifdef __cplusplus
extern "C" {
#endif

// Live HAL adapter for target-owned AMDGPU HSACO emission.
extern const loom_artifact_provider_t loom_amdgpu_artifact_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_AMDGPU_ARTIFACT_PROVIDER_H_
