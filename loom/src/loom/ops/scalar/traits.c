// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/scalar/ops.h"

// Scalar and vector add/sub/mul/fma share wrapping integer semantics. A no-wrap
// flag instead requires a fact that may only hold on the original control path.
// Recompute the owned bit so changing flags can both grant and revoke safety.
loom_trait_flags_t loom_scalar_integer_arithmetic_effective_traits(
    const loom_module_t* module, const loom_op_t* op) {
  (void)module;
  loom_trait_flags_t traits = op->traits & ~LOOM_TRAIT_SAFE_TO_SPECULATE;
  if (!(op->instance_flags &
        (LOOM_INTEGER_OVERFLOW_FLAG_NSW | LOOM_INTEGER_OVERFLOW_FLAG_NUW))) {
    traits |= LOOM_TRAIT_SAFE_TO_SPECULATE;
  }
  return traits;
}
