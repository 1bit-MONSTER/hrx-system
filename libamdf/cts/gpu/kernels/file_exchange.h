// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_FILE_EXCHANGE_H_
#define AMDF_CTS_GPU_KERNELS_FILE_EXCHANGE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::file_exchange {

// One finite invocation owns SQ publication and CQ consumption. All addresses
// are GPU addresses except host_payload, which the Linux I/O consumer uses.
struct alignas(16) Arguments {
  // GPU address of the array of native 64-byte submission entries.
  uint64_t submission_entries;
  // GPU address of the native 32-bit submission tail.
  uint64_t submission_tail;
  // GPU address of the array of native 16-byte completion entries.
  uint64_t completion_entries;
  // GPU address of the native 32-bit completion head.
  uint64_t completion_head;
  // GPU address of the native 32-bit completion tail.
  uint64_t completion_tail;
  // GPU address of the first payload window, after its prefix guard.
  uint64_t payload;
  // GPU address of the terminal summary and complete per-round records.
  uint64_t records;
  // CPU virtual address corresponding to payload, in fixed buffer zero.
  uint64_t host_payload;
  // Returned SQ entry-count mask; the ring starts empty at position zero.
  uint32_t submission_mask;
  // Returned CQ entry-count mask; the ring starts empty at position zero.
  uint32_t completion_mask;
  // Number of causal read, transform, write, and reload rounds.
  uint32_t round_count;
  // Number of 32-bit words in one complete file block and payload window.
  uint32_t word_count;
  // Mask for a power-of-two input bank, followed by the same output capacity.
  uint32_t file_block_mask;
  // Initial cause; each later cause is the preceding reloaded payload word.
  uint32_t seed;
  // Distance in bytes between read, write, and reload payload windows.
  uint32_t payload_stride;
  // Fixed-file index used by all requests, including the error witness.
  uint32_t file_index;
};

// The summary contains completed rounds, terminal result, requests, and cause.
inline constexpr uint32_t kSummaryWordCount = 4;
// Each round retains its input block, cause, write block, and every reload
// word.
inline constexpr uint32_t kRecordHeaderWordCount = 3;
// EOF before a requested region is complete has an explicit terminal result.
inline constexpr int32_t kIncompleteRead = -61;
// Linux READ_FIXED and WRITE_FIXED opcodes, checked against the native UAPI.
inline constexpr uint32_t kReadFixed = 4;
inline constexpr uint32_t kWriteFixed = 5;

inline constexpr std::array<uint32_t, 16> kArgumentByteOffsets = {
    offsetof(Arguments, submission_entries),
    offsetof(Arguments, submission_tail),
    offsetof(Arguments, completion_entries),
    offsetof(Arguments, completion_head),
    offsetof(Arguments, completion_tail),
    offsetof(Arguments, payload),
    offsetof(Arguments, records),
    offsetof(Arguments, host_payload),
    offsetof(Arguments, submission_mask),
    offsetof(Arguments, completion_mask),
    offsetof(Arguments, round_count),
    offsetof(Arguments, word_count),
    offsetof(Arguments, file_block_mask),
    offsetof(Arguments, seed),
    offsetof(Arguments, payload_stride),
    offsetof(Arguments, file_index)};
inline constexpr std::array<uint32_t, 16> kArgumentByteLengths = {
    8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4, 4, 4};
inline constexpr std::array<std::string_view, 16> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "global_buffer", "global_buffer", "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value",      "by_value"};

static_assert(sizeof(Arguments) == 96);

}  // namespace kernels::file_exchange

#endif  // AMDF_CTS_GPU_KERNELS_FILE_EXCHANGE_H_
