// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Two waves exchange separately tagged static and dynamic LDS values.
static __attribute__((address_space(3),
                      loader_uninitialized)) unsigned static_words[128];

[[clang::amdgpu_kernel, gnu::visibility("protected")]]
__attribute__((amdgpu_flat_work_group_size(128, 128))) void lds_exchange(
    __attribute__((address_space(1))) unsigned* output,
    __attribute__((address_space(3))) unsigned* dynamic_words, unsigned seed,
    unsigned dynamic_stride) {
  unsigned local_id = __builtin_amdgcn_workitem_id_x();
  unsigned group_id = __builtin_amdgcn_workgroup_id_x();
  unsigned global_id = group_id * 128 + local_id;
  static_words[local_id] = seed + group_id * 0x01020307u + local_id * 0x1021u;
  if (dynamic_stride != 0) {
    dynamic_words[local_id * dynamic_stride] =
        (seed ^ 0xa5a55a5au) + group_id * 0x01010101u + local_id * 0x0203u +
        dynamic_stride * 0x00100001u;
  }
  __builtin_amdgcn_fence(__ATOMIC_RELEASE, "workgroup", "local");
  __builtin_amdgcn_s_barrier();
  __builtin_amdgcn_fence(__ATOMIC_ACQUIRE, "workgroup", "local");
  unsigned partner = local_id ^ 64u;
  output[global_id * 2] = static_words[partner];
  output[global_id * 2 + 1] = dynamic_stride != 0
                                  ? dynamic_words[partner * dynamic_stride]
                                  : seed ^ (0x5a17c0deu + global_id);
}
