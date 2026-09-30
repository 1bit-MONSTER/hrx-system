// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_TARGET_WASM_H_
#define LOOMC_TARGET_WASM_H_

#include "loomc/target.h"

/// @file
/// WebAssembly compiler package.
///
/// The package compiles functions for WebAssembly SIMD128 and emits canonical
/// WebAssembly binary modules. Compilation and emission require no live device
/// or platform runtime.

#ifdef __cplusplus
extern "C" {
#endif

/// WebAssembly binary module artifact format.
#define LOOMC_ARTIFACT_FORMAT_WASM_BINARY "wasm-binary"

/// Creates a target environment containing the WebAssembly compiler and
/// emitter.
///
/// @param allocator Host allocator used for target-environment storage.
/// @param out_target_environment Receives one retained environment on success.
/// @return OK when the target environment was created.
///
/// @ownership
/// The caller releases the returned reference with
/// `loomc_target_environment_release`. The environment may be shared across
/// compiler instances and worker threads.
LOOMC_API_EXPORT loomc_status_t loomc_target_environment_create_wasm(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOMC_TARGET_WASM_H_
