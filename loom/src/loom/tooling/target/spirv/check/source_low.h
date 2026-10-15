// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_SPIRV_CHECK_SOURCE_LOW_H_
#define LOOM_TOOLING_TARGET_SPIRV_CHECK_SOURCE_LOW_H_

#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

// Lowers source with public SPIR-V feature facts and compares Low IR.
extern const loom_check_emit_provider_t loom_spirv_loom_check_lower_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_SPIRV_CHECK_SOURCE_LOW_H_
