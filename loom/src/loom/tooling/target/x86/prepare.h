// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_X86_PREPARE_H_
#define LOOM_TOOLING_TARGET_X86_PREPARE_H_

#include "loom/target/emit/native/contribution.h"
#include "loom/target/entry_selection.h"

#ifdef __cplusplus
extern "C" {
#endif

// Native callable output detached from the source module and compiler scratch.
typedef struct loom_x86_callable_t {
  // Output-arena-owned external symbol name.
  iree_string_view_t symbol_name;
  // Output-arena-owned code, including entry, body, and return instructions.
  loom_native_section_contribution_t text;
} loom_x86_callable_t;

// Prepares one selected x86 object-function entry using its explicit calling
// convention. The shared compiler owns scheduling, allocation, and transport;
// this boundary admits the ABI signature and supplies fixed entry locations.
// Unsupported signatures emit diagnostics and leave |out_prepared| false.
// The first supported convention is SysV AMD64 with register-passed integer
// and pointer arguments, at most one result, and a spill-free leaf body.
// All output storage belongs to |output_arena|; scratch may be reset on return.
iree_status_t loom_x86_prepare_callable(
    loom_module_t* module, const loom_target_entry_t* entry,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t emitter, iree_arena_allocator_t* scratch_arena,
    iree_arena_allocator_t* output_arena, bool* out_prepared,
    loom_x86_callable_t* out_callable);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_X86_PREPARE_H_
