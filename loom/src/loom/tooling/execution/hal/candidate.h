// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// HAL artifact candidates produced by Loom execution tooling.

#ifndef LOOM_TOOLING_EXECUTION_HAL_CANDIDATE_H_
#define LOOM_TOOLING_EXECUTION_HAL_CANDIDATE_H_

#include "iree/base/api.h"
#include "loom/target/provider.h"
#include "loom/tooling/compile/options.h"
#include "loom/tooling/execution/hal/device_provider.h"
#include "loom/tooling/execution/session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_run_hal_candidate_t {
  // Exact executable target row accepted by the emitted artifact.
  const iree_hal_executable_target_t* executable_target;
  // True when artifact bytes were produced.
  bool compiled;
  // Artifact bytes produced by the provider's core target emitter.
  loom_target_emit_artifact_t artifact;
} loom_run_hal_candidate_t;

// Emits |run_module| to a HAL artifact candidate using the selected |target|
// and |session| target registries. Both are required and borrowed only for the
// duration of this call.
iree_status_t loom_run_hal_candidate_emit_target(
    const loom_device_provider_t* provider, const loom_device_target_t* target,
    loom_run_session_t* session, loom_run_module_t* run_module,
    const loom_compile_options_t* options, iree_allocator_t allocator,
    loom_run_hal_candidate_t* out_candidate);

// Releases all artifact storage owned by |candidate|.
void loom_run_hal_candidate_deinitialize(loom_run_hal_candidate_t* candidate);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_EXECUTION_HAL_CANDIDATE_H_
