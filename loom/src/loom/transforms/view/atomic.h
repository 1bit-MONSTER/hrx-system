// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_VIEW_ATOMIC_H_
#define LOOM_TRANSFORMS_VIEW_ATOMIC_H_

#include "loom/ops/atomic.h"
#include "loom/rewrite/rewriter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds the scalar update of a verified atomic kind. Integer arithmetic wraps;
// floating arithmetic uses the target's scalar mode without fast-math flags.
// Exchange returns |value| directly. Memory atomicity and ordering belong to
// the caller's access sequence.
iree_status_t loom_view_atomic_build_combine(
    loom_builder_t* builder, loom_atomic_kind_t kind, loom_value_id_t old,
    loom_value_id_t value, loom_type_t type, loom_location_id_t location,
    loom_value_id_t* out_value);

// Rewrites a scalar view atomic on proven invocation-private storage to
// ordinary accesses and scalar arithmetic. The caller establishes PRIVATE
// memory-space facts for the view. Compare-exchange compares payload bits, and
// every returning operation preserves the old bits. For noftz addition the
// caller must establish that the selected target's scalar arithmetic preserves
// subnormals. Target-independent callers retain that op.
iree_status_t loom_view_atomic_rewrite_private(loom_rewriter_t* rewriter,
                                               loom_op_t* op);

// Rewrites a scalar atomic RMW/reduction to a bitwise compare-exchange loop.
// The caller supplies an integer or floating-point payload occupying whole
// bytes and establishes compare-exchange support for its width and memory
// space. Arithmetic inherits the target's scalar mode; noftz callers establish
// that it preserves subnormals. The returned old payload retains its exact
// bits, including NaNs.
iree_status_t loom_view_atomic_rewrite_cmpxchg(loom_rewriter_t* rewriter,
                                               loom_op_t* op);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_VIEW_ATOMIC_H_
