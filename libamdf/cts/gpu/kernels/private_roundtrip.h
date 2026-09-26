// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_PRIVATE_ROUNDTRIP_H_
#define AMDF_CTS_GPU_KERNELS_PRIVATE_ROUNDTRIP_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::private_roundtrip {

// Paired with private_roundtrip.loom's 16-byte semantic kernarg layout.
struct alignas(16) Arguments {
  // Global GPU address of the first nine-word output record.
  uint64_t output;
  // Token seed, combined with the global workitem and private slot.
  uint32_t seed;
  // Runtime rotation of each workitem's nine private words.
  uint32_t rotation;
};
static_assert(alignof(Arguments) == 16);
static_assert(sizeof(Arguments) == 16);
static_assert(offsetof(Arguments, output) == 0);
static_assert(offsetof(Arguments, seed) == 8);
static_assert(offsetof(Arguments, rotation) == 12);

// Compiled resource fixtures share this native caller layout.
inline constexpr std::array<uint32_t, 3> kArgumentByteOffsets = {
    offsetof(Arguments, output), offsetof(Arguments, seed),
    offsetof(Arguments, rotation)};
inline constexpr std::array<uint32_t, 3> kArgumentByteLengths = {
    sizeof(Arguments::output), sizeof(Arguments::seed),
    sizeof(Arguments::rotation)};
inline constexpr std::array<std::string_view, 3> kArgumentValueKinds = {
    "global_buffer", "by_value", "by_value"};

// Walks the private-slot permutation cyclically and truncates wider token
// arithmetic only after forming each independently expected output word.
inline std::array<uint32_t, 9> ExpectedRecord(uint32_t workitem, uint32_t seed,
                                              uint32_t rotation) {
  std::array<uint32_t, 9> record;
  const uint32_t local_id = workitem % 64;
  uint32_t selected_slot = (local_id + rotation) % 9;
  for (uint32_t& word : record) {
    const uint64_t value = uint64_t{seed} + uint64_t{workitem} * 0x01020307u +
                           uint64_t{selected_slot} * 0x1021u;
    word = static_cast<uint32_t>(value);
    if (++selected_slot == record.size()) {
      selected_slot = 0;
    }
  }
  return record;
}

}  // namespace kernels::private_roundtrip

#endif  // AMDF_CTS_GPU_KERNELS_PRIVATE_ROUNDTRIP_H_
