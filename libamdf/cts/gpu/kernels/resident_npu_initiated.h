// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_RESIDENT_NPU_INITIATED_H_
#define AMDF_CTS_GPU_KERNELS_RESIDENT_NPU_INITIATED_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::resident_npu_initiated {

// Device arguments for the finite resident protocol, shared by all targets.
struct Arguments {
  // GPU address of the GPU-produced return slot R, including its generation.
  uint64_t request;
  // GPU address of the NPU-produced slot Q, including its generation.
  uint64_t response;
  // GPU address of the separately maintained startup decision.
  uint64_t startup;
  // GPU address of the guarded final-acknowledgement allocation.
  uint64_t control;
  // GPU address of the complete Q1 through Q(N+1) transcript.
  uint64_t records;
  // Number of GPU returns; positive counts require one closing NPU payload.
  uint32_t round_count;
  // Number of 32-bit words in each complete payload.
  uint32_t payload_word_count;
  // Word offset from either slot base to its first payload word.
  uint32_t payload_word_offset;
};

// Semantic wire extent; the caller initializes any compiler fetch padding.
inline constexpr uint32_t kArgumentByteLength =
    offsetof(Arguments, payload_word_offset) + sizeof(uint32_t);
// Independent host layout used to check every compiled product.
inline constexpr std::array<uint32_t, 8> kArgumentByteOffsets = {
    offsetof(Arguments, request),
    offsetof(Arguments, response),
    offsetof(Arguments, startup),
    offsetof(Arguments, control),
    offsetof(Arguments, records),
    offsetof(Arguments, round_count),
    offsetof(Arguments, payload_word_count),
    offsetof(Arguments, payload_word_offset)};
inline constexpr std::array<uint32_t, 8> kArgumentByteLengths = {8, 8, 8, 8,
                                                                 8, 4, 4, 4};
inline constexpr std::array<std::string_view, 8> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "by_value",      "by_value",      "by_value"};

static_assert(kArgumentByteLength == 52);
static_assert(sizeof(Arguments) == 56);

}  // namespace kernels::resident_npu_initiated

#endif  // AMDF_CTS_GPU_KERNELS_RESIDENT_NPU_INITIATED_H_
