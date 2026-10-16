// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <math.h>

int main(void) {
  volatile double input = 0.5;
  const double result = sin(input);
  return result > 0.0 && result < 1.0 ? 0 : 1;
}
