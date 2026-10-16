// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared compact construction of approximate F32 math operations.

#ifndef LOOM_TRANSFORMS_MATH_APPROXIMATE_F32_H_
#define LOOM_TRANSFORMS_MATH_APPROXIMATE_F32_H_

#include "iree/base/api.h"
#include "loom/transforms/math/evaluation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_math_approximate_f32_op_e {
  // Full-domain approximation of 2^x.
  LOOM_MATH_APPROXIMATE_F32_OP_EXP2 = 0,
  // Full-domain approximation of sin(2*pi*x).
  LOOM_MATH_APPROXIMATE_F32_OP_SIN_TURNS = 1,
  // Full-domain approximation of cos(2*pi*x).
  LOOM_MATH_APPROXIMATE_F32_OP_COS_TURNS = 2,
  LOOM_MATH_APPROXIMATE_F32_OP_COUNT_ = 3,
} loom_math_approximate_f32_op_t;

// Builds one approximation in the scalar or vector evaluation lane domain.
iree_status_t loom_math_approximate_f32_build(
    loom_math_evaluation_t* evaluation, loom_math_approximate_f32_op_t op,
    const loom_math_evaluation_value_t* input,
    loom_math_evaluation_value_t* out_value);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_MATH_APPROXIMATE_F32_H_
