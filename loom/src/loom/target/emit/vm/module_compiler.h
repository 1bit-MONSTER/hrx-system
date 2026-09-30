// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// VM whole-module compilation.

#ifndef LOOM_TARGET_EMIT_VM_MODULE_COMPILER_H_
#define LOOM_TARGET_EMIT_VM_MODULE_COMPILER_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Canonical VM binary module emitter.
extern const loom_target_emitter_t loom_vm_module_emitter;

// VM binary module emission composed with the VM target fact type.
extern const loom_target_provider_t loom_vm_module_provider;

// Complete VM compiler provider set containing the target architecture and
// canonical module emitter.
extern const loom_target_provider_set_t loom_vm_compiler_provider_set;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_VM_MODULE_COMPILER_H_
