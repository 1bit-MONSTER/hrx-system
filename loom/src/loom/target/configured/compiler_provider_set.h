// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Complete compiler providers selected by //loom/config/target.

#ifndef LOOM_TARGET_CONFIGURED_COMPILER_PROVIDER_SET_H_
#define LOOM_TARGET_CONFIGURED_COMPILER_PROVIDER_SET_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the immutable build-selected artifact emitter provider set.
const loom_target_provider_set_t* loom_configured_emitter_provider_set(void);

// Returns the immutable configured compiler provider set. The set combines
// every build-selected artifact emitter with its concrete target architecture
// and has process lifetime. Portable command programs use their dedicated
// LoomC construction transaction instead of this configured environment.
const loom_target_provider_set_t* loom_configured_compiler_provider_set(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_CONFIGURED_COMPILER_PROVIDER_SET_H_
