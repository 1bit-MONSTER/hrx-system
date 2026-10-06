// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU generated Workgroup storage accounting.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_WORKGROUP_STORAGE_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_WORKGROUP_STORAGE_H_

#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

// Validates the exact Workgroup storage declarations selected for the current
// source function against its target limit.
iree_status_t loom_amdgpu_validate_workgroup_storage(
    loom_low_lower_context_t* context);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_WORKGROUP_STORAGE_H_
