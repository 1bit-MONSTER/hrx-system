// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_PASS_PROGRAM_STORAGE_H_
#define LOOMC_PASS_PROGRAM_STORAGE_H_

#include "loom/ir/function_version.h"
#include "loom/ir/ir.h"
#include "loom/pass/environment.h"
#include "loom/pass/program.h"
#include "loomc/pass.h"
#include "loomc/result.h"
#include "loomc/workspace.h"
#include "visibility.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the context retained by the public pass program handle.
LOOMC_API_PRIVATE loomc_context_t* loomc_pass_program_context(
    const loomc_pass_program_t* pass_program);

// Returns the immutable Loom pass program owned by the public handle.
LOOMC_API_PRIVATE const loom_pass_program_t*
loomc_pass_program_loom_pass_program(const loomc_pass_program_t* pass_program);

// Runs |pass_program| on one trusted compiler-owned module.
//
// |supplemental_capability| optionally extends the context's ordinary codegen
// pass environment for this invocation. Pass diagnostics are appended to
// |result|; an error diagnostic marks the result failed while returning OK.
LOOMC_API_PRIVATE loomc_status_t loomc_pass_program_run_internal_module(
    loomc_workspace_t* workspace, const loomc_pass_program_t* pass_program,
    loom_module_t* module,
    loom_function_version_owner_t* function_version_owner,
    const loom_pass_environment_capability_t* supplemental_capability,
    loomc_result_t* result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOMC_PASS_PROGRAM_STORAGE_H_
