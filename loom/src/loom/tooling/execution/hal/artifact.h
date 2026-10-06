// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Device-specialized target and artifact pairing for live HAL execution.

#ifndef LOOM_TOOLING_EXECUTION_HAL_ARTIFACT_H_
#define LOOM_TOOLING_EXECUTION_HAL_ARTIFACT_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loadable artifact bytes paired with the exact device executable target that
// selected them. Both fields are borrowed for the device artifact lifetime.
typedef struct loom_device_artifact_t {
  // Exact executable target row borrowed from the active device spec.
  const iree_hal_executable_target_t* executable_target;
  // Loadable artifact accepted by the production HAL loader.
  const loom_target_emit_artifact_t* artifact;
} loom_device_artifact_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_EXECUTION_HAL_ARTIFACT_H_
