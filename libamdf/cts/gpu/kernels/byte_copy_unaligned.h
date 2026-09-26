// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_BYTE_COPY_UNALIGNED_H_
#define AMDF_CTS_GPU_KERNELS_BYTE_COPY_UNALIGNED_H_

#include <cstddef>
#include <cstdint>

namespace kernels::byte_copy_unaligned {

// Paired with byte_copy_unaligned.loom's 36-byte semantic layout. The caller
// checks the compiled segment fits an initialized 64-byte, 16-aligned slot
// and copies only the semantic fields, leaving any fetch padding zeroed.
struct alignas(16) Arguments {
  // Global GPU address of the first source byte.
  uint64_t source;
  // Global GPU address of the first destination byte.
  uint64_t target;
  // Number of disjoint source and destination bytes to copy.
  uint64_t byte_length;
  // Number of workitems in the complete X grid.
  uint32_t grid_size_x;
  // Number of workitems in the complete Y grid.
  uint32_t grid_size_y;
  // Nominal number of workitems along X in each workgroup.
  uint32_t workgroup_size_x;
};
static_assert(alignof(Arguments) == 16);
static_assert(sizeof(Arguments) == 48);
static_assert(offsetof(Arguments, source) == 0);
static_assert(offsetof(Arguments, target) == 8);
static_assert(offsetof(Arguments, byte_length) == 16);
static_assert(offsetof(Arguments, grid_size_x) == 24);
static_assert(offsetof(Arguments, grid_size_y) == 28);
static_assert(offsetof(Arguments, workgroup_size_x) + sizeof(uint32_t) == 36);

}  // namespace kernels::byte_copy_unaligned

#endif  // AMDF_CTS_GPU_KERNELS_BYTE_COPY_UNALIGNED_H_
