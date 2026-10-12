// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Wasm vector memory legalization.

#ifndef LOOM_TARGET_ARCH_WASM_LEGALIZATION_MEMORY_H_
#define LOOM_TARGET_ARCH_WASM_LEGALIZATION_MEMORY_H_

#include "iree/base/api.h"
#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rewrites a dense numeric vector load or store smaller than one v128 into
// power-of-two scalar accesses. The exact vector footprint is covered with at
// most four 8-, 16-, 32-, or 64-bit accesses; vector structural operations
// retain the value in its packed v128 carrier between memory and its users.
iree_status_t loom_wasm_partial_vector_memory_rewrite_op(
    loom_target_legalization_context_t* context, loom_op_t* op,
    bool* out_rewritten);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_WASM_LEGALIZATION_MEMORY_H_
