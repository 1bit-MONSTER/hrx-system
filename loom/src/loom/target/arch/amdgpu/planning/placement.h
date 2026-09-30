// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_PLACEMENT_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_PLACEMENT_H_

#include "loom/codegen/low/placement_recipe.h"
#include "loom/codegen/low/target_binding.h"

#ifdef __cplusplus
extern "C" {
#endif

// Selects measured instruction preferences for the resolved processor,
// subgroup and actual descriptor set. Unqualified forms return an empty view.
loom_low_placement_instruction_preferences_t
loom_amdgpu_placement_instruction_preferences(
    const loom_low_resolved_target_t* target);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_PLACEMENT_H_
