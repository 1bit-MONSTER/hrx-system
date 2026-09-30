// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_VM_PROGRAM_BUILD_H_
#define LOOM_TARGET_ARCH_VM_PROGRAM_BUILD_H_

#include "iree/base/internal/arena.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/error/emitter.h"
#include "loom/target/arch/vm/program.h"
#include "loom/target/function_version.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds one immutable physical VM program plan from a compiler-owned module.
//
// Target selection, signature interning, reference/import/export tables,
// scheduling, allocation, spill materialization, instruction selection,
// branch fixups, and every wire-format limit are resolved before this returns.
// Structured compiler rejection returns OK with |out_accepted| false. On
// acceptance, plan tables are owned by |arena| and remain valid until that
// arena is reset. The immutable function bytecode is owned by
// |bytecode_allocator| so a target writer can retain it in an output without
// copying. The caller must deinitialize the accepted plan; writing the binary
// does not consume plan ownership. Any rejection or failure leaves |out_plan|
// empty.
iree_status_t loom_vm_program_plan_build(
    loom_module_t* module,
    const loom_function_version_list_t* function_versions,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t diagnostic_emitter, iree_arena_allocator_t* arena,
    iree_allocator_t bytecode_allocator, bool* out_accepted,
    loom_vm_program_plan_t* out_plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_VM_PROGRAM_BUILD_H_
