// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

[[loom::kernel, loom::workgroup_count(1, 1, 1), loom::workgroup_size(64, 1, 1)]]
void divide(const unsigned* input, unsigned* output) {
  output[0] = input[0] / input[1];
}
