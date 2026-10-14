// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_RUNTIME_PROGRAM_PARAMETER_LAYOUT_H_
#define LOOM_SERVE_RUNTIME_PROGRAM_PARAMETER_LAYOUT_H_

#include "loom/target/arch/cmd/program.h"

#ifdef __cplusplus
extern "C" {
#endif

// Checks whether independently compiled commands can share already-prepared
// parameter roots. Keys, offsets, extents, bindings and alignment must match;
// workspace and execution order are independent. This external model-source
// boundary does not infer numerical/state compatibility from equal placement.
iree_status_t loom_serve_parameter_layout_check(
    const loom_cmd_program_t* baseline, const loom_cmd_program_t* candidate);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_RUNTIME_PROGRAM_PARAMETER_LAYOUT_H_
