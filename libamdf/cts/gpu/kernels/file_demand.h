// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_FILE_DEMAND_H_
#define AMDF_CTS_GPU_KERNELS_FILE_DEMAND_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::file_demand {

inline constexpr uint32_t kMaximumCredits = 256;
inline constexpr uint32_t kMaximumFileBlocks = 1024;
inline constexpr uint32_t kMaximumDemands = 1024;

// Finite offered demands are independent of I/O completion. One GPU owner
// performs admission, key deduplication, native publication and retirement.
// A separately submitted instance can compute while that owner performs I/O.
struct alignas(16) Arguments {
  // GPU address of native-format SQEs.
  uint64_t submission_entries;
  // GPU address of the selected transport's SQ tail.
  uint64_t submission_tail;
  // GPU address of native-format CQEs.
  uint64_t completion_entries;
  // GPU address of the selected transport's CQ head.
  uint64_t completion_head;
  // GPU address of the selected transport's CQ tail.
  uint64_t completion_tail;
  // GPU address of the first guard-separated payload window.
  uint64_t payload;
  // GPU output address of Summary followed by credit_count Slot objects.
  uint64_t state;
  // GPU address of demand_count records, with immutable offered inputs.
  uint64_t records;
  // GPU output address of file_block_mask + 1 final key-to-slot cells.
  uint64_t keys;
  // GPU address of shared background-work startup and terminal state.
  uint64_t background;
  // CPU virtual address corresponding to payload, inside fixed buffer zero.
  uint64_t host_payload;
  // Native SQ entry count minus one.
  uint32_t submission_mask;
  // Native CQ entry count minus one.
  uint32_t completion_mask;
  // Retired native SQ/CQ position at entry.
  uint32_t initial_position;
  // Independent payload credits, at most kMaximumCredits and below ring
  // capacity.
  uint32_t credit_count;
  // Scheduled logical demands, including duplicates, at most kMaximumDemands.
  uint32_t demand_count;
  // Power-of-two number of words transferred by each physical I/O.
  uint32_t word_count;
  // Power-of-two input-bank capacity minus one, below kMaximumFileBlocks.
  uint32_t file_block_mask;
  // Byte stride between input and reload windows, including guards.
  uint32_t payload_stride;
  // One for immutable reads, three for read/write/reload KV chains.
  uint32_t phase_count;
  // Fixed-file table index; zero is valid, one exercises native error drain.
  uint32_t file_index;
  // Zero for the I/O owner, one for independent arithmetic.
  uint32_t role;
  // Whether the I/O owner waits for the independent instance's startup.
  uint32_t background_enabled;
};

struct Summary {
  // First native error, or zero on complete success.
  int32_t status;
  // Number of physically published SQEs, including short retries.
  uint32_t submitted;
  // Number of native CQEs consumed before returning.
  uint32_t completed;
  // Reference-clock origin of the offered schedule.
  uint32_t begin_tick;
  // Reference-clock time after every accepted ownership path retires.
  uint32_t end_tick;
  // Maximum native submissions awaiting GPU consumption.
  uint32_t peak_outstanding;
  // Logical demands admitted into an allocated or shared credit.
  uint32_t admitted;
  // Logical consumers that released their final payload reference.
  uint32_t consumed;
  // Admitted duplicate readers that issued no new physical read.
  uint32_t deduplicated;
  // Maximum live payload credits, including retained completed reads.
  uint32_t peak_credits;
  // Free-list head, encoded as slot plus one; zero means empty.
  uint32_t free_head;
  // Ready-consumer queue head, encoded as slot plus one.
  uint32_t ready_head;
  // Ready-consumer queue tail, encoded as slot plus one.
  uint32_t ready_tail;
  // Number of currently allocated payload credits.
  uint32_t live_credits;
  // Scratch index of the next offered demand.
  uint32_t next_demand;
  // Scratch next credit to issue, encoded as slot plus one.
  uint32_t issue;
  // Upper word of the same full-width sample that produced begin_tick.
  uint32_t begin_tick_high;
  // Upper word of the same full-width sample that produced end_tick.
  uint32_t end_tick_high;
  // Reserved zero words keep the following slot table sixteen-byte aligned.
  std::array<uint32_t, 2> reserved;
};

struct Slot {
  // Immutable input block selected by the GPU hash.
  uint32_t key;
  // First pending consumer, encoded as record index plus one.
  uint32_t first_reader;
  // Last pending consumer, encoded as record index plus one.
  uint32_t last_reader;
  // Free-list or ready-queue successor, encoded as slot plus one.
  uint32_t next;
  // Current native operation: read zero, write one, reload two.
  uint32_t phase;
  // Positive bytes already completed within that operation.
  uint32_t progress;
  // First publication timestamp of the current physical operation.
  uint32_t submit_tick;
  // Final I/O completion timestamp before consuming logical readers.
  uint32_t ready_tick;
  // Payload generation incremented on each new ownership.
  uint32_t generation;
  // Number of logical references not yet released.
  uint32_t readers;
  // Last physical submission ticket for the retirement oracle.
  uint32_t ticket;
  // Latest full-operation completion timestamp.
  uint32_t phase_end_tick;
  // Stable first record that retains physical-operation timestamps.
  uint32_t leader;
  // Final reader release of the preceding generation, or zero initially.
  uint32_t last_release;
  // Reserved zero words keep each credit on a sixteen-byte boundary.
  std::array<uint32_t, 2> reserved;
};

// Private ownership storage is workgroup-local, not a CPU/GPU shared mailbox.
inline constexpr uint32_t kGroupByteLength =
    sizeof(Summary) + kMaximumCredits * sizeof(Slot) +
    kMaximumFileBlocks * sizeof(uint32_t) +
    kMaximumDemands * 8 * sizeof(uint32_t);

struct Record {
  // Offered time relative to Summary.begin_tick, independent of completion.
  uint32_t arrival_ticks;
  // Input to the GPU's key hash; equal values represent duplicate demand.
  uint32_t key_seed;
  // Deliberate retained-reader interval after native data readiness.
  uint32_t hold_ticks;
  // Next reader of the same backing, encoded as record index plus one.
  uint32_t next;
  // Reference clock when admission acquired or shared a credit.
  uint32_t admitted_tick;
  // Reference clock when this GPU consumer first probed returned data.
  uint32_t ready_tick;
  // Reference clock after this consumer's final read and reference release.
  uint32_t end_tick;
  // Physical payload credit used by this consumer.
  uint32_t slot;
  // Payload generation observed by this consumer.
  uint32_t generation;
  // File key selected by the GPU hash.
  uint32_t key;
  // First payload word observed on final consumption.
  uint32_t first;
  // Key-selected payload word observed on final consumption.
  uint32_t selected;
  // Last payload word observed on final consumption.
  uint32_t last;
  // Combined numerical output from the three payload words.
  uint32_t result;
  // Whether admission shared an existing key instead of issuing a read.
  uint32_t shared;
  // First read's publication time, shared by joined readers.
  uint32_t read_begin;
  // First read's native completion time.
  uint32_t read_end;
  // KV write publication time; zero for immutable lookups.
  uint32_t write_begin;
  // KV write native completion time; zero for immutable lookups.
  uint32_t write_end;
  // KV reload publication time; zero for immutable lookups.
  uint32_t reload_begin;
  // KV reload native completion time; zero for immutable lookups.
  uint32_t reload_end;
  // Final physical submission ticket observed by this consumer.
  uint32_t ticket;
  // Final reader release of the preceding owner of this credit, or zero.
  uint32_t previous_release;
  // Whether the first payload probe and ready timestamp have been recorded.
  uint32_t observed;
};

struct Background {
  // Independent GPU instance has started and retains its arguments.
  uint32_t started;
  // I/O owner has established the common clock origin.
  uint32_t run;
  // I/O owner has completed native and consumer retirement.
  uint32_t stop;
  // Completed independent arithmetic iterations.
  uint32_t iterations;
  // Independently checkable modulo-32-bit arithmetic result.
  uint32_t result;
};

inline constexpr uint32_t kArgumentByteLength = 136;
inline constexpr std::array<uint32_t, 23> kArgumentByteOffsets = {
    offsetof(Arguments, submission_entries),
    offsetof(Arguments, submission_tail),
    offsetof(Arguments, completion_entries),
    offsetof(Arguments, completion_head),
    offsetof(Arguments, completion_tail),
    offsetof(Arguments, payload),
    offsetof(Arguments, state),
    offsetof(Arguments, records),
    offsetof(Arguments, keys),
    offsetof(Arguments, background),
    offsetof(Arguments, host_payload),
    offsetof(Arguments, submission_mask),
    offsetof(Arguments, completion_mask),
    offsetof(Arguments, initial_position),
    offsetof(Arguments, credit_count),
    offsetof(Arguments, demand_count),
    offsetof(Arguments, word_count),
    offsetof(Arguments, file_block_mask),
    offsetof(Arguments, payload_stride),
    offsetof(Arguments, phase_count),
    offsetof(Arguments, file_index),
    offsetof(Arguments, role),
    offsetof(Arguments, background_enabled)};
inline constexpr std::array<uint32_t, 23> kArgumentByteLengths = {
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
inline constexpr std::array<std::string_view, 23> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "global_buffer", "by_value",      "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value"};
static_assert(sizeof(Arguments) == 144);
static_assert(sizeof(Summary) == 80);
static_assert(sizeof(Slot) == 64);
static_assert(sizeof(Record) == 96);
static_assert(sizeof(Background) == 20);

}  // namespace kernels::file_demand

#endif  // AMDF_CTS_GPU_KERNELS_FILE_DEMAND_H_
