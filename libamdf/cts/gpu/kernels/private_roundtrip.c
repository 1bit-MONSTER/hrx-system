// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Each workitem initializes and reads all nine private words. Volatile AS5
// accesses retain actual private storage in the optimized compiler artifact.
[[clang::amdgpu_kernel, gnu::visibility("protected")]]
__attribute__((amdgpu_flat_work_group_size(64, 64))) void private_roundtrip(
    __attribute__((address_space(1))) unsigned* output, unsigned seed,
    unsigned rotation) {
  unsigned local_id = __builtin_amdgcn_workitem_id_x();
  unsigned global_id = __builtin_amdgcn_workgroup_id_x() * 64 + local_id;
  volatile unsigned private_words[9];
  volatile __attribute__((address_space(5))) unsigned* words =
      (volatile __attribute__((address_space(5))) unsigned*)private_words;
  for (unsigned slot = 0; slot < 9; ++slot) {
    words[slot] = seed + global_id * 0x01020307u + slot * 0x1021u;
  }
  for (unsigned slot = 0; slot < 9; ++slot) {
    unsigned selected_slot = (slot + local_id + rotation) % 9;
    output[global_id * 9 + slot] = words[selected_slot];
  }
}
