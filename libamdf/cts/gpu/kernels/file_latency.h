// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_FILE_LATENCY_H_
#define AMDF_CTS_GPU_KERNELS_FILE_LATENCY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::file_latency {

// One finite completion-driven owner, with up to four independent causal
// streams. Native and host-relayed transports use the identical product.
struct alignas(16) Arguments {
  // GPU address of native-format SQEs, initially zero outside authored fields.
  uint64_t submission_entries;
  // GPU address of the selected transport's SQ tail.
  uint64_t submission_tail;
  // GPU address of native-format CQEs.
  uint64_t completion_entries;
  // GPU address of the selected transport's CQ head.
  uint64_t completion_head;
  // GPU address of the selected transport's CQ tail.
  uint64_t completion_tail;
  // GPU address of the first guarded input window.
  uint64_t payload;
  // GPU address of Summary followed by depth Slot objects.
  uint64_t state;
  // GPU address of depth * round_count * phase_count Record objects.
  uint64_t records;
  // CPU virtual address of payload, within fixed buffer zero.
  uint64_t host_payload;
  // Native power-of-two SQ capacity minus one.
  uint32_t submission_mask;
  // Native power-of-two CQ capacity minus one.
  uint32_t completion_mask;
  // Retired native SQ/CQ position at entry; counters need not begin at zero.
  uint32_t initial_position;
  // Independent streams, from one through four and below both ring capacities.
  uint32_t depth;
  // Complete consumer generations per stream.
  uint32_t round_count;
  // Power-of-two number of 32-bit words in each transfer.
  uint32_t word_count;
  // Power-of-two immutable input bank capacity minus one.
  uint32_t file_block_mask;
  // Initial cause; each slot adds its ordinal before hashing.
  uint32_t seed;
  // Byte distance between guard-separated input and reload windows.
  uint32_t payload_stride;
  // One for lookups, three for read/write/reload block round trips.
  uint32_t phase_count;
  // Minimum reference-clock ticks between consumers; gap cases use depth one.
  uint32_t gap_ticks;
  // Fixed-file table index; ordinary work uses zero, the error witness one.
  uint32_t file_index;
};

struct Summary {
  // First native error, or zero after complete success.
  int32_t status;
  // Physical requests, including positive short retries.
  uint32_t submitted;
  // Physical completions consumed before return, including error drain.
  uint32_t completed;
  // Reference-clock sample before initial submission preparation.
  uint32_t begin_tick;
  // Reference-clock sample after final response consumption and record stores.
  uint32_t end_tick;
  // Upper word of the same full-width sample that produced begin_tick.
  uint32_t begin_tick_high;
  // Upper word of the same full-width sample that produced end_tick.
  uint32_t end_tick_high;
  // Reserved zero word keeps the following slot table sixteen-byte aligned.
  uint32_t reserved;
};

struct Slot {
  // Response-derived cause determining this consumer's input block.
  uint32_t cause;
  // Completed consumers in this stream.
  uint32_t round;
  // Read zero, write one, reload two.
  uint32_t phase;
  // Successfully transferred bytes within the current operation.
  uint32_t progress;
};

struct Record {
  // Stream owning the operation and its payload credit.
  uint32_t slot;
  // Consumer generation within that stream.
  uint32_t round;
  // Read zero, write one, reload two.
  uint32_t phase;
  // Immutable input block selected by the cause.
  uint32_t key;
  // Cause before consuming this operation's returned bytes.
  uint32_t cause;
  // Reference-clock sample immediately before first SQ-tail publication.
  uint32_t begin_tick;
  // Reference-clock sample after payload probes and result stores complete.
  uint32_t end_tick;
  // Completed length; zero until a full operation is successfully consumed.
  int32_t result;
  // First payload word; writes leave this zero.
  uint32_t first;
  // Key-selected payload word; writes leave this zero.
  uint32_t selected;
  // Last payload word; writes leave this zero.
  uint32_t last;
  // Last physical submission ticket, including a positive short retry.
  uint32_t ticket;
};

inline constexpr uint32_t kArgumentByteLength = 120;
inline constexpr uint32_t kMaximumDepth = 4;
inline constexpr std::array<uint32_t, 21> kArgumentByteOffsets = {
    offsetof(Arguments, submission_entries),
    offsetof(Arguments, submission_tail),
    offsetof(Arguments, completion_entries),
    offsetof(Arguments, completion_head),
    offsetof(Arguments, completion_tail),
    offsetof(Arguments, payload),
    offsetof(Arguments, state),
    offsetof(Arguments, records),
    offsetof(Arguments, host_payload),
    offsetof(Arguments, submission_mask),
    offsetof(Arguments, completion_mask),
    offsetof(Arguments, initial_position),
    offsetof(Arguments, depth),
    offsetof(Arguments, round_count),
    offsetof(Arguments, word_count),
    offsetof(Arguments, file_block_mask),
    offsetof(Arguments, seed),
    offsetof(Arguments, payload_stride),
    offsetof(Arguments, phase_count),
    offsetof(Arguments, gap_ticks),
    offsetof(Arguments, file_index)};
inline constexpr std::array<uint32_t, 21> kArgumentByteLengths = {
    8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
inline constexpr std::array<std::string_view, 21> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value"};

static_assert(sizeof(Arguments) == 128);
static_assert(offsetof(Arguments, gap_ticks) == 112);
static_assert(sizeof(Summary) == 32);
static_assert(sizeof(Slot) == 16);
static_assert(sizeof(Record) == 48);

}  // namespace kernels::file_latency

#endif  // AMDF_CTS_GPU_KERNELS_FILE_LATENCY_H_
