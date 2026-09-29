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
#include <string_view>

namespace kernels::lds_exchange {

// Paired with lds_exchange.loom's 12-byte semantic kernarg layout. Native
// callers initialize the complete 16-byte slot used for scalar fetches.
struct alignas(16) Arguments {
  // Global GPU address of the first exchange-token/position-stamp output pair.
  uint64_t output;
  // Token seed, combined with workgroup and partner lane.
  uint32_t seed;
};
static_assert(alignof(Arguments) == 16);
static_assert(sizeof(Arguments) == 16);
static_assert(offsetof(Arguments, output) == 0);
static_assert(offsetof(Arguments, seed) == 8);

// Semantic bytes copied into a zero-initialized native argument segment.
inline constexpr uint32_t kArgumentByteLength =
    offsetof(Arguments, seed) + sizeof(Arguments::seed);

// All compiled target variants share this native caller layout.
inline constexpr std::array<uint32_t, 2> kArgumentByteOffsets = {
    offsetof(Arguments, output), offsetof(Arguments, seed)};
inline constexpr std::array<uint32_t, 2> kArgumentByteLengths = {
    sizeof(Arguments::output), sizeof(Arguments::seed)};
inline constexpr std::array<std::string_view, 2> kArgumentValueKinds = {
    "global_buffer", "by_value"};

// Derives the other wave's lane independently, forms its tokens with wider
// arithmetic, then applies the kernel's unsigned 32-bit wrapping.
inline std::array<uint32_t, 2> ExpectedRecord(uint32_t workitem,
                                              uint32_t seed) {
  const uint32_t group = workitem / 128;
  const uint32_t lane = workitem % 128;
  const uint32_t partner = lane < 64 ? lane + 64 : lane - 64;
  const uint64_t static_value = uint64_t{seed} + uint64_t{group} * 0x01020307u +
                                uint64_t{partner} * 0x1021u;
  return {static_cast<uint32_t>(static_value),
          seed ^ static_cast<uint32_t>(uint64_t{0x5a17c0deu} + workitem)};
}

}  // namespace kernels::lds_exchange

#endif  // AMDF_CTS_GPU_KERNELS_LDS_EXCHANGE_H_
