// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// SPIR-V compiler provider composition.

#ifndef LOOM_TARGET_ARCH_SPIRV_COMPILER_PROVIDER_H_
#define LOOM_TARGET_ARCH_SPIRV_COMPILER_PROVIDER_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// SPIR-V binary emission associated with the SPIR-V target family.
extern const loom_target_provider_t loom_spirv_compiler_provider;

// Complete SPIR-V compiler provider set containing the target architecture and
// canonical SPIR-V module emitter.
extern const loom_target_provider_set_t loom_spirv_compiler_provider_set;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_SPIRV_COMPILER_PROVIDER_H_
