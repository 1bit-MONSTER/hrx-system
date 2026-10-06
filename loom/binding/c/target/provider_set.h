// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Binding-private target package contributions used by configured joins.

#ifndef LOOM_BINDING_C_TARGET_PROVIDER_SET_H_
#define LOOM_BINDING_C_TARGET_PROVIDER_SET_H_

#include "loom/binding/c/src/visibility.h"
#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the immutable compiler provider set owned by the AMDGPU package.
LOOMC_API_PRIVATE const loom_target_provider_set_t* loomc_amdgpu_provider_set(
    void);

// Returns the immutable compiler provider set owned by the SPIR-V package.
LOOMC_API_PRIVATE const loom_target_provider_set_t* loomc_spirv_provider_set(
    void);

// Returns the immutable compiler provider set owned by the VM package.
LOOMC_API_PRIVATE const loom_target_provider_set_t* loomc_vm_provider_set(void);

// Returns the immutable compiler provider set owned by the Wasm package.
LOOMC_API_PRIVATE const loom_target_provider_set_t* loomc_wasm_provider_set(
    void);

// Returns the immutable compiler provider set owned by the XDNA package.
LOOMC_API_PRIVATE const loom_target_provider_set_t* loomc_xdna_provider_set(
    void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_BINDING_C_TARGET_PROVIDER_SET_H_
