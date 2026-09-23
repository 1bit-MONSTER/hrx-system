// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_LDS_EXCHANGE_H_
#define AMDF_CTS_GPU_KERNELS_LDS_EXCHANGE_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace kernels::lds_exchange {

// Semantic arguments occupy 20 bytes. The aligned slot backs the scalar load
// through byte 23 without making its unused lane another argument.
struct alignas(16) Arguments {
  // Global GPU address of the first static/dynamic output pair.
  uint64_t output;
  // Group-segment byte offset of the dynamic region after static LDS.
  uint32_t dynamic_offset;
  // Token seed, combined with workgroup and partner lane.
  uint32_t seed;
  // Dynamic LDS element spacing, or zero to select the static-only branch.
  uint32_t dynamic_stride;
};
static_assert(alignof(Arguments) == 16);
static_assert(sizeof(Arguments) == 32);
static_assert(offsetof(Arguments, output) == 0);
static_assert(offsetof(Arguments, dynamic_offset) == 8);
static_assert(offsetof(Arguments, seed) == 12);
static_assert(offsetof(Arguments, dynamic_stride) == 16);
static_assert(offsetof(Arguments, dynamic_stride) + sizeof(uint32_t) == 20);

// Derives the other wave's lane independently, forms its tokens with wider
// arithmetic, then applies the kernel's unsigned 32-bit wrapping.
inline std::array<uint32_t, 2> ExpectedRecord(uint32_t workitem, uint32_t seed,
                                              uint32_t dynamic_stride) {
  const uint32_t group = workitem / 128;
  const uint32_t lane = workitem % 128;
  const uint32_t partner = lane < 64 ? lane + 64 : lane - 64;
  const uint64_t static_value = uint64_t{seed} + uint64_t{group} * 0x01020307u +
                                uint64_t{partner} * 0x1021u;
  std::array<uint32_t, 2> record;
  record[0] = static_cast<uint32_t>(static_value);
  if (dynamic_stride != 0) {
    const uint64_t dynamic_value =
        uint64_t{seed ^ 0xa5a55a5au} + uint64_t{group} * 0x01010101u +
        uint64_t{partner} * 0x0203u + uint64_t{dynamic_stride} * 0x00100001u;
    record[1] = static_cast<uint32_t>(dynamic_value);
  } else {
    record[1] = seed ^ static_cast<uint32_t>(uint64_t{0x5a17c0deu} + workitem);
  }
  return record;
}

}  // namespace kernels::lds_exchange

#endif  // AMDF_CTS_GPU_KERNELS_LDS_EXCHANGE_H_
