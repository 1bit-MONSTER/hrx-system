// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_TARGET_X86_H_
#define LOOMC_TARGET_X86_H_

#include "loomc/target.h"

/// @file
/// Native x86 target environment creation.

#ifdef __cplusplus
extern "C" {
#endif

/// Creates a target environment containing the native x86 compiler and
/// emitter.
///
/// @param allocator Host allocator used for target-environment storage.
/// @param out_target_environment Receives one retained target environment on
/// success.
/// @return OK when the target environment was created.
///
/// @ownership
/// The caller owns the returned reference and releases it with
/// `loomc_target_environment_release`.
LOOMC_API_EXPORT loomc_status_t loomc_target_environment_create_x86(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOMC_TARGET_X86_H_
