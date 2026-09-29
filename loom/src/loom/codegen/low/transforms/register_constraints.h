// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Materialization of descriptor register constraints in ordinary Low IR.

#ifndef LOOM_CODEGEN_LOW_TRANSFORMS_REGISTER_CONSTRAINTS_H_
#define LOOM_CODEGEN_LOW_TRANSFORMS_REGISTER_CONSTRAINTS_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"
#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

const loom_pass_info_t* loom_low_materialize_register_constraints_pass_info(
    void);

// Gives independently owned results the copies needed by two-address machine
// instructions. Existing ownership ties remain part of the source contract.
iree_status_t loom_low_materialize_register_constraints_run(
    loom_pass_t* pass, loom_module_t* module, loom_func_like_t function);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_TRANSFORMS_REGISTER_CONSTRAINTS_H_
