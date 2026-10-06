// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Test-only schedule observation for authored Low programs.

#ifndef LOOM_TOOLS_LOOM_CHECK_TEST_SCHEDULE_PROVIDER_H_
#define LOOM_TOOLS_LOOM_CHECK_TEST_SCHEDULE_PROVIDER_H_

#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const loom_check_emit_provider_t loom_check_test_schedule_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_LOOM_CHECK_TEST_SCHEDULE_PROVIDER_H_
