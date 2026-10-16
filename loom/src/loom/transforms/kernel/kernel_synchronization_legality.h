// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Kernel synchronization legality gate.
//
// This pass proves whole-function synchronization contracts after target
// specialization and required inlining. Local op verifiers independently
// check types, attributes, and SSA contracts.

#ifndef LOOM_TRANSFORMS_KERNEL_SYNCHRONIZATION_LEGALITY_H_
#define LOOM_TRANSFORMS_KERNEL_SYNCHRONIZATION_LEGALITY_H_

#include "iree/base/api.h"
#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

const loom_pass_info_t* loom_kernel_synchronization_legality_pass_info(void);

iree_status_t loom_kernel_synchronization_legality_run(
    loom_pass_t* pass, loom_module_t* module, loom_func_like_t function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_KERNEL_SYNCHRONIZATION_LEGALITY_H_
