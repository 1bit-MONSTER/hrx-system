// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_CLEANUP_CANONICALIZE_H_
#define LOOM_TRANSFORMS_CLEANUP_CANONICALIZE_H_

#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Pass facades
//===----------------------------------------------------------------------===//

// Returns immutable metadata for the canonicalize pass.
const loom_pass_info_t* loom_canonicalize_pass_info(void);

// Returns immutable metadata for the pre-legalization combine pass.
const loom_pass_info_t* loom_combine_pass_info(void);

// Creates canonicalizer state from a textual option dictionary.
iree_status_t loom_canonicalizer_pass_create(loom_pass_t* pass,
                                             iree_string_view_t options);

// Applies universal simplifications without recomposing representations chosen
// by legalization. Safe for source, intermediate, and final cleanup. Resolves
// pass-scoped facts and math policy and records changes and statistics.
iree_status_t loom_canonicalize_run(loom_pass_t* pass, loom_module_t* module,
                                    loom_func_like_t function);

// Applies source combines and universal simplifications in one fixed-point
// session. Combines may introduce representations that require target
// legalization; this pass belongs before that boundary. Shares the ordinary
// canonicalizer's worklist, facts, scratch storage, and change accounting.
iree_status_t loom_combine_run(loom_pass_t* pass, loom_module_t* module,
                               loom_func_like_t function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CLEANUP_CANONICALIZE_H_
