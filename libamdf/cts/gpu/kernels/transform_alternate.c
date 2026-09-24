// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Same argument and access contract as transform.c, with distinct arithmetic.
// Complete 64-workitem groups keep either image valid during retired-code
// reuse.
[[clang::amdgpu_kernel, gnu::visibility("protected")]]
__attribute__((amdgpu_flat_work_group_size(64, 64))) void aql_transform(
    const __attribute__((address_space(1))) unsigned* input,
    __attribute__((address_space(1))) unsigned* output, unsigned count,
    unsigned addend) {
  unsigned index =
      __builtin_amdgcn_workgroup_id_x() * 64 + __builtin_amdgcn_workitem_id_x();
  if (index < count) {
    output[index] = input[index] * 5u + addend;
  }
}
