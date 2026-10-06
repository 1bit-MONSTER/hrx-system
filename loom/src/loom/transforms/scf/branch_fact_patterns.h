// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_SCF_BRANCH_FACT_PATTERNS_H_
#define LOOM_TRANSFORMS_SCF_BRANCH_FACT_PATTERNS_H_

#include "loom/rewrite/pattern_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

// SCF branch-edge fact patterns. Compiler compositions register these for the
// ordered region-initialization phase and the universal pre-fold phase so
// nested roots initialize outer-to-inner and later root mutations remain
// incrementally maintained.
extern const loom_rewrite_pattern_provider_t
    loom_scf_branch_fact_pattern_provider;

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_SCF_BRANCH_FACT_PATTERNS_H_
