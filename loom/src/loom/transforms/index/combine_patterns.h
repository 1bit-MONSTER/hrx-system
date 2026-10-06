// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_INDEX_COMBINE_PATTERNS_H_
#define LOOM_TRANSFORMS_INDEX_COMBINE_PATTERNS_H_

#include "loom/rewrite/pattern_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

// Source-combine patterns contributed by the index dialect.
extern const loom_rewrite_pattern_provider_t
    loom_index_source_combine_pattern_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_INDEX_COMBINE_PATTERNS_H_
