// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_X86_VECTOR_INSERT_H_
#define LOOM_TARGET_ARCH_X86_VECTOR_INSERT_H_

#include "iree/base/api.h"
#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

// Legalizes a scalar insertion into native x86 vector packets. Returns false
// through |out_rewritten| when the result has no complete packet expansion or
// native dynamic-insert representation.
iree_status_t loom_x86_vector_insert_legalize(
    loom_target_legalization_context_t* context, loom_op_t* op,
    bool* out_rewritten);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_X86_VECTOR_INSERT_H_
