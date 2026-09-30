// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_VM_PROVIDER_H_
#define LOOM_TARGET_ARCH_VM_PROVIDER_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// VM target definitions, descriptor tables, and shared-compiler policies.
// Compiler tools producing VM images also compose the canonical module emitter;
// linking this provider alone does not link program planning, the VM runtime,
// or its text tools.
extern const loom_target_provider_t loom_vm_target_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_VM_PROVIDER_H_
