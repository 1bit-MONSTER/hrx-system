// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Cleanup pattern providers selected by the full Loom compiler build.

#ifndef LOOM_TRANSFORMS_CLEANUP_CONFIGURED_H_
#define LOOM_TRANSFORMS_CLEANUP_CONFIGURED_H_

#include "loom/transforms/cleanup/patterns.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the immutable full-compiler cleanup provider selection.
const loom_cleanup_pattern_provider_set_t*
loom_cleanup_configured_pattern_provider_set(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_CLEANUP_CONFIGURED_H_
