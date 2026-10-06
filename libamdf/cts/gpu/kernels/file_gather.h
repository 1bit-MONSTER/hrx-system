// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_FILE_GATHER_H_
#define AMDF_CTS_GPU_KERNELS_FILE_GATHER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::file_gather {

// A finite single-owner event loop has three independently progressing I/Os.
// Nine guard-separated payload windows form input, write and reload banks.
// Addresses are GPU VAs except host_payload, used by Linux fixed-buffer I/O.
struct alignas(16) Arguments {
  // GPU address of native 64-byte SQEs, initially zero.
  uint64_t submission_entries;
  // GPU address of the native SQ tail, initially zero.
  uint64_t submission_tail;
  // GPU address of native 16-byte CQEs.
  uint64_t completion_entries;
  // GPU address of the native CQ head, initially zero.
  uint64_t completion_head;
  // GPU address of the native CQ tail, initially zero.
  uint64_t completion_tail;
  // GPU address of payload window zero, after its prefix guard.
  uint64_t payload;
  // GPU address of the device-initialized State.
  uint64_t state;
  // GPU address of (3 + 2 * peer_round_count) complete consumer records.
  uint64_t records;
  // GPU address of request_capacity Request entries for the retirement oracle.
  uint64_t requests;
  // CPU VA corresponding to payload, contained in fixed buffer zero.
  uint64_t host_payload;
  // Native SQ entry-count mask; capacity must be at least three.
  uint32_t submission_mask;
  // Native CQ entry-count mask; capacity must be at least three.
  uint32_t completion_mask;
  // Causal round trips in each independent stream, in [2, 33].
  uint32_t peer_round_count;
  // Number of 32-bit words per file block, in [1, 16384].
  uint32_t word_count;
  // Initial cause; each slot adds 17 times its ordinal.
  uint32_t seed;
  // Distance between payload windows in bytes, including each trailing guard.
  uint32_t payload_stride;
  // Source slot retained by the delayed duplicate consumer, in [0, 2].
  uint32_t held_slot;
  // Zero for success, one/three/four for read/write/reload fixed-file errors,
  // or two for a positive short input followed by EOF.
  uint32_t fault;
  // Available Request entries, in [3, kRequestCapacity]; exhaustion drains.
  uint32_t request_capacity;
};

inline constexpr uint32_t kSlotCount = 3;
inline constexpr uint32_t kWindowCount = 9;
inline constexpr uint32_t kRequestCapacity = 256;
inline constexpr uint32_t kInputBlockCount = 16;
inline constexpr uint32_t kOutputBlockCount = 128;
inline constexpr uint32_t kRecordHeaderWords = 8;

// Device counters describe ownership rather than elapsed time.
struct Summary {
  // First negative result, -ENODATA for EOF, or -EOVERFLOW for a full journal.
  int32_t status;
  // Number of release-published SQEs.
  uint32_t submitted;
  // Number of acquired and consumed CQEs, including drain after failure.
  uint32_t completed;
  // Maximum published-minus-consumed request count.
  uint32_t peak_outstanding;
  // First submissions of unique input reads, excluding positive short retries.
  uint32_t unique_reads;
  // Requests joined to an existing key without issuing another read.
  uint32_t deduplicated;
  // Peer reloads after their first round while the held source stays live.
  uint32_t held_peer_reloads;
  // Complete peer round trips; both peers finish before the delayed reader.
  uint32_t peer_completed;
  // Immutable SQ tail when the first error was observed; zero on success.
  uint32_t failure_tail;
  // CQ position when failure stops useful processing; zero on success.
  uint32_t failure_completion;
  // Set when the first duplicate reader finishes and leaves one reference.
  uint32_t held_ready;
  // Slot found by the device key lookup for the duplicate request.
  uint32_t duplicate_slot;
};

// One source credit and at most one I/O belong to each slot. The device owns
// this table throughout execution; the host inspects it only after retirement.
struct Slot {
  // Ready/in-flight read 0/1, write 2/3, reload 4/5, held 6, or retired 7.
  uint32_t phase;
  // Logical consumer within this stream; the held stream has three consumers.
  uint32_t consumer;
  // Cause selecting the source and biasing its consumer's arithmetic.
  uint32_t cause;
  // Immutable input file block retained until the final reader releases it.
  uint32_t key;
  // Pending source readers, independent of native CQ ownership.
  uint32_t references;
  // Completed bytes of the current logical read, write or reload.
  uint32_t progress;
  // Latest submitted ticket, paired with slot identity in native user_data.
  uint32_t ticket;
  // Generation of input backing; the duplicate shares generation zero.
  uint32_t generation;
};

struct State {
  // Device-initialized progress and terminal observations.
  Summary summary;
  // Three independent source lifetimes.
  std::array<Slot, kSlotCount> slots;
};

// Indexed by submission ticket, not CQ arrival order. The bounded journal is
// an oracle output; exhaustion stops publication and drains accepted I/O.
struct Request {
  // Owner slot encoded in native user_data's low word.
  uint32_t slot;
  // Ready phase at publication: read 0, write 2, or reload 4.
  uint32_t phase;
  // Logical consumer borrowing this operation.
  uint32_t consumer;
  // Cause at publication, independent of completion order.
  uint32_t cause;
  // File block, before adding progress to the byte offset.
  uint32_t block;
  // Byte offset into the logical block and payload window.
  uint32_t progress;
  // Remaining bytes requested by this SQE.
  uint32_t length;
  // Exact native result, including short, zero and negative completions.
  int32_t result;
  // Zero-based CQ position at consumption, not the submission ticket.
  uint32_t completion;
  // Published tail at consumption, used to verify stop-and-drain ordering.
  uint32_t submitted_at_completion;
  // Source references before this operation's payload preparation.
  uint32_t references;
  // Exact native CQ flags.
  uint32_t flags;
  // Number of consumed CQEs before publication, proving dependency order.
  uint32_t completion_frontier;
};

inline constexpr std::array<uint32_t, 19> kArgumentByteOffsets = {
    offsetof(Arguments, submission_entries),
    offsetof(Arguments, submission_tail),
    offsetof(Arguments, completion_entries),
    offsetof(Arguments, completion_head),
    offsetof(Arguments, completion_tail),
    offsetof(Arguments, payload),
    offsetof(Arguments, state),
    offsetof(Arguments, records),
    offsetof(Arguments, requests),
    offsetof(Arguments, host_payload),
    offsetof(Arguments, submission_mask),
    offsetof(Arguments, completion_mask),
    offsetof(Arguments, peer_round_count),
    offsetof(Arguments, word_count),
    offsetof(Arguments, seed),
    offsetof(Arguments, payload_stride),
    offsetof(Arguments, held_slot),
    offsetof(Arguments, fault),
    offsetof(Arguments, request_capacity)};
inline constexpr std::array<uint32_t, 19> kArgumentByteLengths = {
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4};
inline constexpr std::array<std::string_view, 19> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value"};

inline constexpr uint32_t kArgumentByteLength =
    offsetof(Arguments, request_capacity) + sizeof(uint32_t);
static_assert(sizeof(Arguments) == 128);
static_assert(sizeof(Summary) == 48);
static_assert(sizeof(Slot) == 32);
static_assert(sizeof(State) == 144);
static_assert(sizeof(Request) == 52);

}  // namespace kernels::file_gather

#endif  // AMDF_CTS_GPU_KERNELS_FILE_GATHER_H_
