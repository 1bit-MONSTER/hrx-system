// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_VM_ARTIFACT_EMITTER_H_
#define LOOM_TOOLING_TARGET_VM_ARTIFACT_EMITTER_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// VM binary artifact emission from prepared target-low modules. Compose with
// loom_vm_target_provider for source lowering and target IR registration.
extern const loom_target_provider_t loom_vm_artifact_emitter_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_VM_ARTIFACT_EMITTER_H_
