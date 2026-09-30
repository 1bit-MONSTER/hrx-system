// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Portable command provider plus concrete target providers selected by
// //loom/config/target.

#ifndef LOOM_TARGET_CONFIGURED_PROVIDER_SET_H_
#define LOOM_TARGET_CONFIGURED_PROVIDER_SET_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the immutable configured target provider set. The portable command
// provider is always present because command programs are compiler products
// independent of the selected concrete target architectures.
const loom_target_provider_set_t* loom_configured_target_provider_set(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_CONFIGURED_PROVIDER_SET_H_
