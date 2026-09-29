// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_X86_CHECK_LOOM_CHECK_H_
#define LOOM_TOOLING_TARGET_X86_CHECK_LOOM_CHECK_H_

#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

// Checks native callable bytes through the production preparation path.
extern const loom_check_emit_provider_t loom_x86_callable_check_emit_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_X86_CHECK_LOOM_CHECK_H_
