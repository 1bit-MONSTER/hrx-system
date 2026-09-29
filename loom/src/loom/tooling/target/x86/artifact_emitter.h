// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_X86_ARTIFACT_EMITTER_H_
#define LOOM_TOOLING_TARGET_X86_ARTIFACT_EMITTER_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ordinary x86-64 ELF object emission. Compose with the x86 architecture
// provider for source lowering and target registration. Function ABI selection
// is explicit and independent of the chosen instruction-set profile.
extern const loom_target_provider_t loom_x86_artifact_emitter_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_X86_ARTIFACT_EMITTER_H_
