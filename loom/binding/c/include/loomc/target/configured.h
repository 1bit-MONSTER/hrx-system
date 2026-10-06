// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_TARGET_CONFIGURED_H_
#define LOOMC_TARGET_CONFIGURED_H_

#include "loomc/target.h"

/// @file
/// Build-configured compiler target package.
///
/// This optional package joins every artifact-capable target extension selected
/// by the Loom build configuration. Programs needing only one target family
/// should link that family's package directly instead.

#ifdef __cplusplus
extern "C" {
#endif

/// Creates an environment containing every configured compiler target.
///
/// The environment contains only target packages linked into this optional
/// join. It does not probe devices or load platform runtimes.
///
/// @param allocator Host allocator used for target-environment storage.
/// @param out_target_environment Receives one retained environment on success.
/// @return OK when the target environment was created.
///
/// @ownership
/// The caller releases the returned reference with
/// `loomc_target_environment_release`. The environment may be shared across
/// compiler instances and worker threads.
LOOMC_API_EXPORT loomc_status_t loomc_target_environment_create_configured(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOMC_TARGET_CONFIGURED_H_
