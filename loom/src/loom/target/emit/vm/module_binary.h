// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// VM module binary emission from a planned physical program.

#ifndef LOOM_TARGET_EMIT_VM_MODULE_BINARY_H_
#define LOOM_TARGET_EMIT_VM_MODULE_BINARY_H_

#include "iree/base/api.h"
#include "iree/base/byte_sequence.h"
#include "loom/target/arch/vm/program.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits one immutable VM bytecode image from |plan|.
//
// Program planning has finalized all semantic and wire-format facts. This
// function only lays out sections and may fail when output storage cannot be
// allocated. The result retains the plan's immutable function bytecode between
// writer-owned prefix and suffix ranges instead of copying it. On success, the
// caller owns |out_binary|.
iree_status_t loom_vm_program_emit_binary(const loom_vm_program_plan_t* plan,
                                          iree_allocator_t allocator,
                                          iree_byte_sequence_t** out_binary);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_VM_MODULE_BINARY_H_
