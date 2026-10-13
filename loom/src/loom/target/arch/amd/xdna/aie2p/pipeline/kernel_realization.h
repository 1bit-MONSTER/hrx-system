// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AIE2P finite realization of source kernel entries.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_PIPELINE_KERNEL_REALIZATION_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_PIPELINE_KERNEL_REALIZATION_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"
#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Replaces one supported source kernel with an equivalent finite pipeline.
// Unsupported kernel contracts emit XDNA/051 at the source definition.
iree_status_t loom_aie2p_kernel_realize(loom_pass_t* pass,
                                        loom_module_t* module,
                                        loom_func_like_t function);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_PIPELINE_KERNEL_REALIZATION_H_
