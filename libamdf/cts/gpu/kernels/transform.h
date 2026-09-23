// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_TRANSFORM_H_
#define AMDF_CTS_GPU_KERNELS_TRANSFORM_H_

#include <cstddef>
#include <cstdint>

namespace kernels::transform {

// Paired with transform.c's compiler metadata. Alignment padding belongs to
// the allocation; the kernel consumes only the first 24 bytes.
struct alignas(16) Arguments {
  // GPU address of the first input word.
  uint64_t input;
  // GPU address of the first output word.
  uint64_t output;
  // Number of words the kernel may read and write.
  uint32_t count;
  // Unsigned scalar added after multiplication, modulo 2^32.
  uint32_t addend;
};
static_assert(offsetof(Arguments, input) == 0);
static_assert(offsetof(Arguments, output) == 8);
static_assert(offsetof(Arguments, count) == 16);
static_assert(offsetof(Arguments, addend) + sizeof(uint32_t) == 24);
static_assert(sizeof(Arguments) == 32);

}  // namespace kernels::transform

#endif  // AMDF_CTS_GPU_KERNELS_TRANSFORM_H_
