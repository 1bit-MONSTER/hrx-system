// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_RESIDENT_CHANNELS_H_
#define AMDF_CTS_GPU_KERNELS_RESIDENT_CHANNELS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::resident_channels {

// Device arguments for the finite resident protocol, shared by all targets.
struct Arguments {
  // GPU address of the request slots, starting with slot zero's generation.
  uint64_t request;
  // GPU address of the response slots, starting with slot zero's generation.
  uint64_t response;
  // GPU address of the separately maintained startup decision.
  uint64_t startup;
  // GPU address of the guarded final-acknowledgement allocation.
  uint64_t control;
  // GPU address of the per-generation transcript.
  uint64_t records;
  // Number of peer-channel exchanges between the held channel's two requests.
  uint32_t peer_round_count;
  // First request's causal input; subsequent inputs come from NPU responses.
  uint32_t seed;
  // Number of 32-bit words in each complete request and response.
  uint32_t payload_word_count;
  // Word offset from either slot base to its first payload word.
  uint32_t payload_word_offset;
  // Logical channel held during the peer sequence: zero or one.
  uint32_t held_channel;
  // Distance in bytes between the two channel bases, aligned to 64 bytes.
  uint32_t channel_byte_stride;
};

// Semantic wire extent; the caller initializes any compiler fetch padding.
inline constexpr uint32_t kArgumentByteLength =
    offsetof(Arguments, channel_byte_stride) + sizeof(uint32_t);
// Independent host layout used to check every compiled product.
inline constexpr std::array<uint32_t, 11> kArgumentByteOffsets = {
    offsetof(Arguments, request),
    offsetof(Arguments, response),
    offsetof(Arguments, startup),
    offsetof(Arguments, control),
    offsetof(Arguments, records),
    offsetof(Arguments, peer_round_count),
    offsetof(Arguments, seed),
    offsetof(Arguments, payload_word_count),
    offsetof(Arguments, payload_word_offset),
    offsetof(Arguments, held_channel),
    offsetof(Arguments, channel_byte_stride)};
inline constexpr std::array<uint32_t, 11> kArgumentByteLengths = {
    8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4};
inline constexpr std::array<std::string_view, 11> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value"};

static_assert(kArgumentByteLength == 64);
static_assert(sizeof(Arguments) == 64);

}  // namespace kernels::resident_channels

#endif  // AMDF_CTS_GPU_KERNELS_RESIDENT_CHANNELS_H_
