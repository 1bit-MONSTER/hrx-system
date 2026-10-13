// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// loom-check provider for canonical C++ export.

#ifndef LOOM_TOOLING_EXPORT_CXX_LOOM_CHECK_H_
#define LOOM_TOOLING_EXPORT_CXX_LOOM_CHECK_H_

#include "loom/tools/loom-check/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Canonical C++ emit and external-tool requirement provider package.
extern const loom_check_provider_t loom_cxx_export_check_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_EXPORT_CXX_LOOM_CHECK_H_
