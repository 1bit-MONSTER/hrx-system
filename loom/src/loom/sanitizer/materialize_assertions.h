// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Materializes semantic sanitizer assertions into executable source IR.

#ifndef LOOM_SANITIZER_MATERIALIZE_ASSERTIONS_H_
#define LOOM_SANITIZER_MATERIALIZE_ASSERTIONS_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"
#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns metadata for the semantic sanitizer assertion materializer.
const loom_pass_info_t* loom_sanitizer_materialize_assertions_pass_info(void);

// Expands value, operation, and layout assertions inside |function| into
// ordinary source predicates followed by kernel.assert. Checked value and view
// results become pure assume/refine aliases after the executable boundary.
// Assertions outside dispatchable kernels or requiring unavailable runtime
// layout information produce structured lowering diagnostics.
iree_status_t loom_sanitizer_materialize_assertions_run(
    loom_pass_t* pass, loom_module_t* module, loom_func_like_t function);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SANITIZER_MATERIALIZE_ASSERTIONS_H_
