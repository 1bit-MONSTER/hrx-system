// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/spirv/compiler_provider.h"

#include "loom/target/arch/spirv/facts.h"
#include "loom/target/arch/spirv/provider.h"
#include "loom/target/emit/spirv/module_compiler.h"

const loom_target_provider_t loom_spirv_compiler_provider = {
    .emitter_list =
        {
            .values =
                (const loom_target_emitter_t* const[]){
                    &loom_spirv_module_emitter},
            .count = 1,
        },
    .canonical_kernel_emitter = &loom_spirv_module_emitter,
    .canonical_kernel_fact_type = &loom_spirv_target_fact_type,
};

static const loom_target_provider_t* const kLoomSpirvCompilerProviders[] = {
    &loom_spirv_target_provider,
    &loom_spirv_compiler_provider,
};

const loom_target_provider_set_t loom_spirv_compiler_provider_set = {
    .providers = kLoomSpirvCompilerProviders,
    .provider_count = IREE_ARRAYSIZE(kLoomSpirvCompilerProviders),
};
