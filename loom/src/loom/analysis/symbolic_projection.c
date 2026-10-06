// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/symbolic_projection.h"

#include "iree/base/internal/math.h"

bool loom_symbolic_projection_divide(const loom_symbolic_projection_t* input,
                                     int64_t divisor,
                                     loom_symbolic_projection_t* output) {
  if (input->modulus != 0 && input->modulus % divisor != 0) {
    return false;
  }
  int64_t combined_divisor = 0;
  if (!iree_checked_mul_i64(input->divisor, divisor, &combined_divisor)) {
    return false;
  }
  *output = *input;
  output->divisor = combined_divisor;
  output->modulus = input->modulus / divisor;
  return true;
}

bool loom_symbolic_projection_remainder(const loom_symbolic_projection_t* input,
                                        int64_t modulus,
                                        loom_symbolic_projection_t* output) {
  if (input->modulus != 0 && input->modulus % modulus != 0) {
    return false;
  }
  *output = *input;
  output->modulus = modulus;
  return true;
}

bool loom_symbolic_projection_equal(const loom_symbolic_projection_t* left,
                                    const loom_symbolic_projection_t* right) {
  return left->value_id == right->value_id && left->scale == right->scale &&
         left->offset == right->offset && left->divisor == right->divisor &&
         left->modulus == right->modulus;
}
