// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/program.h"

void loom_vm_program_plan_deinitialize(loom_vm_program_plan_t* plan) {
  iree_byte_sequence_release(plan->function_bytecode);
  *plan = (loom_vm_program_plan_t){0};
}
