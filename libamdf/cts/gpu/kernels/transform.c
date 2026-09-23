// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Finite integer transform with no runtime services, private or group memory.
// The launch uses complete 64-workitem groups; count guards the final group.
[[clang::amdgpu_kernel, gnu::visibility("protected")]]
__attribute__((amdgpu_flat_work_group_size(64, 64))) void aql_transform(
    const __attribute__((address_space(1))) unsigned* input,
    __attribute__((address_space(1))) unsigned* output, unsigned count,
    unsigned addend) {
  unsigned index =
      __builtin_amdgcn_workgroup_id_x() * 64 + __builtin_amdgcn_workitem_id_x();
  if (index < count) {
    output[index] = input[index] * 3u + addend;
  }
}
