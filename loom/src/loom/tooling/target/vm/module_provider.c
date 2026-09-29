// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/vm/module_provider.h"

#include "loom/target/arch/vm/ops/ops.h"
#include "loom/target/emit/vm/module_compiler.h"

const loom_target_provider_t loom_vm_module_provider = {
    .emitter_list =
        {
            .values =
                (const loom_target_emitter_t* const[]){&loom_vm_module_emitter},
            .count = 1,
        },
    .canonical_module_emitter = &loom_vm_module_emitter,
    .canonical_module_fact_type = &loom_vm_target_fact_type,
};
