// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_EMIT_NATIVE_X86_FUNCTION_BODY_H_
#define LOOM_TARGET_EMIT_NATIVE_X86_FUNCTION_BODY_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/codegen/low/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// Detached physical body of one scalar native function. All return sites
// transfer to the byte immediately following the body, where the callable
// envelope restores preserved registers and returns to its caller.
typedef struct loom_x86_function_body_t {
  // Complete body bytes owned by the output arena, including resolved branches.
  iree_const_byte_span_t contents;
  // Architectural GPR write union, including allocation moves and implicit
  // instruction writes. The callable envelope intersects it with the ABI's
  // preserved set; argument reads alone do not require saving a register.
  uint16_t written_registers;
} loom_x86_function_body_t;

// Encodes a successful shared emission frame with a validated scalar callable
// signature (zero or one result). Entry placement and reservation of RSP are
// owned by callable preparation. |result_register| is the ABI return location.
// The shared schedule and allocator own control flow, destructive ties, and
// physical move sequences. This routine owns only native byte layout and
// branch displacement patching. Unsupported target instructions, allocation,
// and native format limits may fail; compiler-owned tables are trusted.
// |scratch_arena| may be reset on return; |out_body| borrows only
// |output_arena|.
iree_status_t loom_x86_function_body_encode(
    const loom_low_emission_frame_t* frame, uint8_t result_register,
    iree_arena_allocator_t* scratch_arena, iree_arena_allocator_t* output_arena,
    loom_x86_function_body_t* out_body);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_X86_FUNCTION_BODY_H_
